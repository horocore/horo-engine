#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "RuntimeUiPaintProjectionInternal.h"

#include <algorithm>
#include <limits>
#include <new>

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
                const auto &payload = input.components[index].payload;
                if (const auto *kind = UiElementCatalog::Find(row->source->type.Value());
                    !kind || std::holds_alternative<UiVisualIndicatorDraw>(payload) ||
                    (kind->surface && std::holds_alternative<UiSolidDraw>(payload)))
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

        /** @brief Admits one authored element after its parent in the pinned canvas traversal. */
        Result<Row> PrepareRow(const UiReloadCanvas &canvas, std::span<const UiDocumentElement> elements, std::span<const Row> parents,
                               const UiElementHandle handle) {
            const auto record = canvas.tree.Get(handle);
            if (record.HasError())
                return Result<Row>::Failure(record.ErrorValue());
            const auto element = std::ranges::find(elements, record.Value().id, &UiDocumentElement::id);
            if (element == elements.end() || !UiElementCatalog::Find(element->type.Value()))
                return Result<Row>::Failure(MakeError(UiErrors::HandleMalformed));
            Row row{.source = std::to_address(element), .element = handle, .end = static_cast<std::uint32_t>(parents.size() + 1)};
            if (const auto parentHandle = record.Value().parent; parentHandle.IsValid()) {
                const auto parent = std::ranges::find(parents, parentHandle, &Row::element);
                if (parent == parents.end())
                    return Result<Row>::Failure(MakeError(UiErrors::HandleMalformed));
                row.parent = static_cast<std::uint32_t>(parent - parents.begin());
            }
            row.stateIndicators = (canvas.focus && canvas.focus->Find(element->id).HasValue()) ||
                                  std::ranges::find(canvas.controls, element->id, &UiReloadControl::id) != canvas.controls.end();
            return Result<Row>::Success(row);
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
        const auto elements = generation.Instance().Elements();
        for (const auto handle : std::span{order}.first(traversed.Value())) {
            auto row = PrepareRow(*canvas, elements, rows, handle);
            if (row.HasError())
                return Result<void>::Failure(row.ErrorValue());
            rows.push_back(std::move(row).Value());
        }
        for (std::size_t index = rows.size(); index-- > 0;)
            if (rows[index].parent != NoUiRenderIndex)
                rows[rows[index].parent].end = std::max(rows[rows[index].parent].end, rows[index].end);
        if (const auto bound = BindComponents(rows, declaration); bound.HasError())
            return bound;
        const auto commands = declaration.components.size() + std::ranges::count_if(rows, [](const Row &row) {
            return UiElementCatalog::Find(row.source->type.Value())->surface;
        }) + std::ranges::count(rows, true, &Row::stateIndicators);
        return commands <= descriptor.limits.commands ? Result<void>::Success()
                                                      : Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
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

    /** @copydoc RuntimeUiPaintProjection::ForkForView */
    Result<std::unique_ptr<RuntimeUiPaintProjection>> RuntimeUiPaintProjection::ForkForView(const RuntimeUiService &service,
                                                                                            UiRenderViewId view) const {
        using Forked = Result<std::unique_ptr<RuntimeUiPaintProjection>>;
        auto current = service.Acquire(storage_->instance);
        if (current.HasError())
            return Forked::Failure(current.ErrorValue());
        if (&current.Value().Get()->Instance() != storage_->expectedInstance)
            return Forked::Failure(MakeError(UiErrors::RevisionStale));
        auto descriptor = storage_->descriptor;
        descriptor.view = view;
        try {
            return Create(service, storage_->instance, storage_->declaration, descriptor);
        } catch (const std::bad_alloc &) {
            return Forked::Failure(MakeError(UiErrors::CapacityExceeded));
        }
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
        const auto geometry = frame.FindViewGeometry(storage_->descriptor.view);
        if (!geometry)
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        const auto &layout = geometry->layout.Descriptor();
        const auto &tree = storage_->canvas->tree;
        if (layout.instance != tree.Instance() || layout.canvas != tree.Canvas() || layout.sources.tree != tree.Revision() ||
            layout.sources.document != tree.SourceDocumentRevision())
            return Result<UiRenderSnapshot>::Failure(MakeError(UiErrors::RevisionStale));
        if (const auto built = storage_->Build({frame.Styles(), geometry->layout, geometry->clipping}); built.HasError())
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
