#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "RuntimeUiPaintProjectionInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;
    using PaintProjectionDetail::Row;

    namespace {
        /** @brief Keeps semantic input failures in the existing canonical UI error domain. */
        Result<void> InvalidSource() {
            return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
        }

        /** @brief Resolves a source component once, outside ordinary frame work. */
        Result<void> BindComponents(std::vector<Row> &rows, const RuntimeUiPaintDeclaration &input) {
            const auto rowFor = [&rows](const UiElementId id) {
                return std::ranges::find_if(rows, [id](const Row &row) {
                    return row.source->id == id;
                });
            };
            for (std::size_t index = 0; index < input.components.size(); ++index) {
                auto row = rowFor(input.components[index].element);
                if (row == rows.end() || row->component != NoUiRenderIndex)
                    return InvalidSource();
                if (const auto *kind = UiElementCatalog::Find(row->source->type.Value());
                    !kind || (kind->surface && std::holds_alternative<UiSolidDraw>(input.components[index].payload)))
                    return InvalidSource();
                row->component = static_cast<std::uint32_t>(index);
            }
            for (std::size_t index = 0; index < input.masks.size(); ++index) {
                auto row = rowFor(input.masks[index].element);
                if (row == rows.end() || row->authoredMask != NoUiRenderIndex)
                    return InvalidSource();
                row->authoredMask = static_cast<std::uint32_t>(index);
            }
            for (const auto element : input.isolated) {
                auto row = rowFor(element);
                if (row == rows.end() || row->isolate)
                    return InvalidSource();
                row->isolate = true;
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc RuntimeUiPaintProjection::Storage::AdmitRows */
    Result<void> RuntimeUiPaintProjection::Storage::AdmitRows() {
        const auto &generation = *(source.Get() ? source.Get() : candidate);
        expectedInstance = &generation.Instance();
        instance = generation.Instance().InstanceId();
        for (const auto &authoredCandidate : generation.Canvases())
            if (authoredCandidate.id == declaration.canvas)
                canvas = &authoredCandidate;
        const auto descriptors = generation.Instance().Canvases();
        const auto authoredCanvas = std::ranges::find(descriptors, declaration.canvas, &UiCanvasDescriptor::id);
        if (!canvas || authoredCanvas == descriptors.end())
            return InvalidSource();
        authored = std::to_address(authoredCanvas);
        std::vector<UiElementHandle> order(descriptor.limits.transforms);
        const auto traversed = canvas->tree.Preorder(order);
        if (traversed.HasError())
            return Result<void>::Failure(traversed.ErrorValue());
        rows.reserve(traversed.Value());
        for (std::size_t index = 0; index < traversed.Value(); ++index) {
            const auto record = canvas->tree.Get(order[index]);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            const auto elements = generation.Instance().Elements();
            const auto element = std::ranges::find_if(elements, [&](const auto &entry) {
                return entry.id == record.Value().id;
            });
            if (element == elements.end() || !UiElementCatalog::Find(element->type.Value()))
                return InvalidSource();
            Row row{.source = std::to_address(element), .element = order[index], .end = static_cast<std::uint32_t>(index + 1)};
            if (record.Value().parent.IsValid()) {
                const auto parent = std::ranges::find_if(rows, [&](const auto &entry) {
                    return entry.element == record.Value().parent;
                });
                if (parent == rows.end())
                    return InvalidSource();
                row.parent = static_cast<std::uint32_t>(parent - rows.begin());
            }
            rows.push_back(row);
        }
        for (std::size_t index = rows.size(); index-- > 0;)
            if (rows[index].parent != NoUiRenderIndex)
                rows[rows[index].parent].end = std::max(rows[rows[index].parent].end, rows[index].end);
        return BindComponents(rows, declaration);
    }

    /** @copydoc RuntimeUiPaintProjection::Create */
    Result<std::unique_ptr<RuntimeUiPaintProjection>> RuntimeUiPaintProjection::Create(const RuntimeUiService &service,
                                                                                       const RuntimeUiInstanceId instance,
                                                                                       RuntimeUiPaintDeclaration declaration,
                                                                                       const UiRenderExtractorDescriptor &descriptor) {
        using Created = Result<std::unique_ptr<RuntimeUiPaintProjection>>;
        auto pin = service.Acquire(instance);
        if (pin.HasError())
            return Created::Failure(pin.ErrorValue());
        auto prepared = PrepareReload(*pin.Value().Get(), std::move(declaration), descriptor);
        if (prepared.HasError())
            return prepared;
        prepared.Value()->storage_->source = std::move(pin).Value();
        prepared.Value()->storage_->candidate = nullptr;
        return prepared;
    }

    /** @copydoc RuntimeUiPaintProjection::PrepareReload */
    Result<std::unique_ptr<RuntimeUiPaintProjection>> RuntimeUiPaintProjection::PrepareReload(
        const UiReloadGeneration &candidate, RuntimeUiPaintDeclaration declaration, const UiRenderExtractorDescriptor &descriptor) {
        using Created = Result<std::unique_ptr<RuntimeUiPaintProjection>>;
        if (const auto &source = candidate.Instance();
            declaration.document != source.DocumentId() || declaration.revision != source.DocumentRevision() ||
            declaration.components.size() > descriptor.limits.commands || declaration.masks.size() > descriptor.limits.masks ||
            declaration.isolated.size() > descriptor.limits.groups || declaration.textRuns.size() > descriptor.limits.textRuns ||
            declaration.glyphs.size() > descriptor.limits.glyphs || declaration.resources.size() > descriptor.limits.resources)
            return Created::Failure(MakeError(UiErrors::HandleMalformed));
        auto extractor = UiRenderExtractor::Create(descriptor);
        if (extractor.HasError())
            return Created::Failure(extractor.ErrorValue());
        auto storage = std::make_unique<Storage>(UiReloadLease{}, std::move(declaration), std::move(extractor).Value(), descriptor);
        storage->candidate = &candidate;
        if (const auto rows = storage->AdmitRows(); rows.HasError())
            return Created::Failure(rows.ErrorValue());
        storage->commands.reserve(descriptor.limits.commands);
        storage->clips.reserve(descriptor.limits.clips);
        storage->masks.reserve(descriptor.limits.masks);
        storage->groups.reserve(descriptor.limits.groups);
        storage->transforms.reserve(descriptor.limits.transforms);
        storage->candidate = nullptr;
        return Created::Success(std::make_unique<RuntimeUiPaintProjection>(ConstructionKey{}, std::move(storage)));
    }

    /** @copydoc RuntimeUiPaintProjection::RuntimeUiPaintProjection */
    RuntimeUiPaintProjection::RuntimeUiPaintProjection(ConstructionKey, std::unique_ptr<Storage> storage) noexcept
        : storage_(std::move(storage)) {}

    /** @copydoc RuntimeUiPaintProjection::Canvas */
    const UiCanvasDescriptor &RuntimeUiPaintProjection::Canvas() const noexcept {
        return *storage_->authored;
    }

    /** @copydoc RuntimeUiPaintProjection::~RuntimeUiPaintProjection */
    RuntimeUiPaintProjection::~RuntimeUiPaintProjection() = default;

    /** @copydoc RuntimeUiPaintProjection::ExtractPrototype */
    Result<UiRenderSnapshot> RuntimeUiPaintProjection::ExtractPrototype(const UiReloadGeneration &source,
                                                                        const Runtime::UiAnimationSourcePresentation &baseline) {
        if (&source.Instance() != storage_->expectedInstance || storage_->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::RevisionStale));
        const auto &layout = baseline.Layout().Descriptor();
        const auto &tree = storage_->canvas->tree;
        if (layout.instance != tree.Instance() || layout.canvas != tree.Canvas() || layout.sources.tree != tree.Revision() ||
            layout.sources.document != tree.SourceDocumentRevision())
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::RevisionStale));
        if (auto built = storage_->Build({baseline.Styles(), baseline.Layout(), baseline.Clipping()}); built.HasError())
            return Result<UiRenderSnapshot>::Failure(built.ErrorValue());
        const UiRenderSnapshotDescriptor descriptor{layout.instance,
                                                    layout.canvas,
                                                    layout.document,
                                                    layout.sources.document,
                                                    layout.sources.tree,
                                                    layout.interaction,
                                                    UiRenderSnapshotRevision::Create(storage_->revision + 1).Value(),
                                                    storage_->descriptor.view,
                                                    storage_->descriptor.limits};
        const auto &input = storage_->declaration;
        auto extracted = storage_->extractor.ExtractPrototype(tree, descriptor,
                                                              {storage_->commands, input.textRuns, input.glyphs, storage_->clips,
                                                               storage_->masks, storage_->transforms, input.resources, storage_->groups});
        if (extracted.HasValue())
            ++storage_->revision;
        return extracted;
    }

    /** @copydoc RuntimeUiPaintProjection::Extract */
    Result<UiRenderSnapshot> RuntimeUiPaintProjection::Extract(const RuntimeUiService &service, const UiAnimationFrameLease &frame) {
        auto current = service.Acquire(storage_->instance);
        if (current.HasError())
            return Result<UiRenderSnapshot>::Failure(current.ErrorValue());
        if (&current.Value().Get()->Instance() != storage_->expectedInstance || !frame.IsValid() ||
            storage_->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::RevisionStale));
        storage_->source = std::move(current).Value();
        const auto &layout = frame.Layout().Descriptor();
        const auto &tree = storage_->canvas->tree;
        if (layout.instance != tree.Instance() || layout.canvas != tree.Canvas() || layout.sources.tree != tree.Revision() ||
            layout.sources.document != tree.SourceDocumentRevision())
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::RevisionStale));
        if (const auto built = storage_->Build({frame.Styles(), frame.Layout(), frame.Clipping()}); built.HasError())
            return Result<UiRenderSnapshot>::Failure(built.ErrorValue());
        UiRenderSnapshotDescriptor descriptor{layout.instance,
                                              layout.canvas,
                                              layout.document,
                                              layout.sources.document,
                                              layout.sources.tree,
                                              layout.interaction,
                                              UiRenderSnapshotRevision::Create(storage_->revision + 1).Value(),
                                              storage_->descriptor.view,
                                              storage_->descriptor.limits};
        const auto &input = storage_->declaration;
        auto extracted = storage_->extractor.Extract(tree, descriptor,
                                                     {storage_->commands, input.textRuns, input.glyphs, storage_->clips, storage_->masks,
                                                      storage_->transforms, input.resources, storage_->groups},
                                                     frame);
        if (extracted.HasValue())
            ++storage_->revision;
        return extracted;
    }
}  // namespace Horo::Application::Internal
