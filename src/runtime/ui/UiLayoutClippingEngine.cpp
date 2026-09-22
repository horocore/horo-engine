#include "UiLayoutClippingInternal.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    using UiLayoutClippingInternal::Add;
    using UiLayoutClippingInternal::Bottom;
    using UiLayoutClippingInternal::CheckedCast;
    using UiLayoutClippingInternal::ClampOffset;
    using UiLayoutClippingInternal::DesiredOffset;
    using UiLayoutClippingInternal::Failure;
    using UiLayoutClippingInternal::NoIndex;
    using UiLayoutClippingInternal::OffsetDelta;
    using UiLayoutClippingInternal::Right;
    using UiLayoutClippingInternal::Translate;
    using UiLayoutClippingInternal::Union;

    UiLayoutClipEngine::Storage::Storage(const UiLayoutClipEngineDescriptor &source) : descriptor(source) {
        slots.reserve(source.concurrentSnapshots);
        for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
            slots.push_back(std::make_shared<UiLayoutClipSnapshot::Storage>(source));

        recordLookup.reserve(source.elementCapacity);
        parents.reserve(source.elementCapacity);
        clipIndexes.reserve(source.elementCapacity);
        ownClipIndexes.reserve(source.elementCapacity);
        scrollIndexes.reserve(source.elementCapacity);
        path.reserve(source.elementCapacity);
        translations.reserve(source.elementCapacity);
        offsets.reserve(source.elementCapacity);
        minimumOffsets.reserve(source.elementCapacity);
        maximumOffsets.reserve(source.elementCapacity);
        viewports.reserve(source.elementCapacity);
        contents.reserve(source.elementCapacity);
        candidateRecords.reserve(source.elementCapacity);
        candidateClips.reserve(source.clipCapacity);
        candidateScrolls.reserve(source.scrollCapacity);
    }

    UiLayoutClipEngine::Storage::~Storage() {
        ReleaseCurrent();
    }

    void UiLayoutClipEngine::Storage::ReleaseCurrent() noexcept {
        if (current) {
            current->leases.fetch_sub(1);
            current.reset();
        }
    }

    std::shared_ptr<UiLayoutClipSnapshot::Storage> UiLayoutClipEngine::Storage::TryAcquire() noexcept {
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const auto index = (nextSlot + offset) % slots.size();
            if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                continue;
            nextSlot = (index + 1) % slots.size();
            return slots[index];
        }
        return {};
    }

    std::uint32_t UiLayoutClipEngine::Storage::FindRecord(const std::span<const UiLayoutRecord> records,
                                                          const UiElementHandle element) const noexcept {
        const auto found = std::ranges::lower_bound(recordLookup, element, {}, [&records](const std::uint32_t index) {
            return records[index].element;
        });
        return found != recordLookup.end() && records[*found].element == element ? *found : NoIndex;
    }

    Result<void> UiLayoutClipEngine::Storage::BuildParentIndex(const UiElementTree &tree, const std::span<const UiLayoutRecord> records) {
        recordLookup.resize(records.size());
        for (std::uint32_t index = 0; index < records.size(); ++index)
            recordLookup[index] = index;
        std::ranges::sort(recordLookup, {}, [&records](const std::uint32_t index) {
            return records[index].element;
        });
        if (std::ranges::adjacent_find(recordLookup, [&records](const std::uint32_t left, const std::uint32_t right) {
            return records[left].element == records[right].element;
        }) != recordLookup.end())
            return Failure(UiErrors::LayoutClipInvalid);

        parents.assign(records.size(), NoIndex);
        for (std::uint32_t index = 0; index < records.size(); ++index) {
            const auto source = tree.Get(records[index].element);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            if (!source.Value().parent.IsValid())
                continue;
            const auto parent = FindRecord(records, source.Value().parent);
            if (parent == NoIndex || parent >= index)
                return Failure(UiErrors::LayoutClipSourceStale);
            parents[index] = parent;
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildScrollBounds(const std::span<const UiLayoutRecord> records,
                                                                const std::span<const UiLayoutClipDescriptor> descriptors) {
        clipIndexes.assign(records.size(), NoIndex);
        ownClipIndexes.assign(records.size(), NoIndex);
        scrollIndexes.assign(records.size(), NoIndex);
        translations.assign(records.size(), {});
        offsets.assign(records.size(), {});
        minimumOffsets.assign(records.size(), {});
        maximumOffsets.assign(records.size(), {});
        viewports.resize(records.size());
        contents.resize(records.size());
        candidateClips.clear();
        candidateScrolls.clear();

        for (std::uint32_t index = 0; index < records.size(); ++index) {
            const auto &arrangement = records[index].arrangement;
            if (!arrangement.IsValid())
                return Failure(UiErrors::LayoutClipInvalid);
            viewports[index] = arrangement.contentBox;
            const auto content = Union(arrangement.contentBox, arrangement.overflow);
            if (content.HasError())
                return Result<void>::Failure(content.ErrorValue());
            contents[index] = content.Value();
            if (descriptors[index].overflow != UiLayoutOverflowPolicy::Scroll)
                continue;

            const auto minimumX =
                CheckedCast(std::min<std::int64_t>(0, static_cast<std::int64_t>(contents[index].origin.x) - viewports[index].origin.x));
            const auto minimumY =
                CheckedCast(std::min<std::int64_t>(0, static_cast<std::int64_t>(contents[index].origin.y) - viewports[index].origin.y));
            const auto maximumX = CheckedCast(std::max<std::int64_t>(0, Right(contents[index]) - Right(viewports[index])));
            const auto maximumY = CheckedCast(std::max<std::int64_t>(0, Bottom(contents[index]) - Bottom(viewports[index])));
            if (minimumX.HasError() || minimumY.HasError() || maximumX.HasError() || maximumY.HasError())
                return Failure(UiErrors::LayoutClipInvalid);
            minimumOffsets[index] = {minimumX.Value(), minimumY.Value()};
            maximumOffsets[index] = {maximumX.Value(), maximumY.Value()};
            const auto x = ClampOffset(descriptors[index].scrollOffset.x, minimumX.Value(), maximumX.Value());
            const auto y = ClampOffset(descriptors[index].scrollOffset.y, minimumY.Value(), maximumY.Value());
            if (x.HasError() || y.HasError())
                return Failure(UiErrors::LayoutClipInvalid);
            offsets[index] = {x.Value(), y.Value()};
            if (candidateScrolls.size() == descriptor.scrollCapacity)
                return Failure(UiErrors::CapacityExceeded);
            scrollIndexes[index] = static_cast<std::uint32_t>(candidateScrolls.size());
            candidateScrolls.emplace_back(records[index].element, viewports[index], contents[index], offsets[index], minimumOffsets[index],
                                          maximumOffsets[index]);
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::ValidateUpdate(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                             const std::span<const UiLayoutRecord> records,
                                                             const UiLayoutClipUpdateRequest &request) const {
        if (const auto sourceResult = ValidateSource(tree, source, records, request); sourceResult.HasError())
            return sourceResult;
        if (const auto elementsResult = ValidateElements(records, request); elementsResult.HasError())
            return elementsResult;
        return ValidateBringIntoView(tree, source, request.bringIntoView);
    }

    Result<void> UiLayoutClipEngine::Storage::ValidateSource(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                             const std::span<const UiLayoutRecord> records,
                                                             const UiLayoutClipUpdateRequest &request) const {
        if (!source.sources.IsValid() || !source.interaction.IsValid() || records.empty() || records.size() > descriptor.elementCapacity ||
            request.elements.size() != records.size())
            return Failure(UiErrors::LayoutClipInvalid);
        if (source.instance != descriptor.instance || source.canvas != descriptor.canvas || source.document != descriptor.document ||
            tree.State() != UiElementTreeState::Active || tree.Instance() != descriptor.instance || tree.Canvas() != descriptor.canvas ||
            tree.SourceDocument() != descriptor.document || tree.SourceDocumentRevision() != source.sources.document ||
            tree.Revision() != source.sources.tree)
            return Failure(UiErrors::LayoutClipSourceStale);
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::ValidateElements(const std::span<const UiLayoutRecord> records,
                                                               const UiLayoutClipUpdateRequest &request) const {
        for (std::uint32_t index = 0; index < records.size(); ++index) {
            if (!request.elements[index].IsValid() || request.elements[index].element != records[index].element)
                return Failure(UiErrors::LayoutClipInvalid);
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::ValidateBringIntoView(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                                    const std::optional<UiFocusBringIntoViewRequest> &request) const {
        if (!request.has_value())
            return Result<void>::Success();
        const auto &bring = *request;
        if (!bring.IsValid())
            return Failure(UiErrors::LayoutClipInvalid);
        if (bring.owner.instance != source.instance || bring.owner.canvas != source.canvas || bring.owner.document != source.document ||
            bring.owner.documentRevision != source.sources.document || bring.owner.treeRevision != source.sources.tree ||
            bring.owner.interaction != source.interaction)
            return Failure(UiErrors::LayoutClipSourceStale);
        const auto target = tree.Get(bring.target.element);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        if (target.Value().id != bring.target.id)
            return Failure(UiErrors::LayoutClipSourceStale);
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildRevealPath(const std::uint32_t target) {
        path.clear();
        for (std::uint32_t currentElement = target; currentElement != NoIndex; currentElement = parents[currentElement]) {
            if (path.size() == descriptor.elementCapacity)
                return Failure(UiErrors::LayoutClipInvalid);
            path.push_back(currentElement);
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::SetRevealOffset(const std::uint32_t scrollElement, const UiFocusBringIntoViewPolicy policy,
                                                              const UiLogicalRect target) {
        const auto desiredX = DesiredOffset(policy, target, viewports[scrollElement], true);
        const auto desiredY = DesiredOffset(policy, target, viewports[scrollElement], false);
        if (desiredX.HasError() || desiredY.HasError())
            return Failure(UiErrors::LayoutClipInvalid);
        const auto nextX = policy == UiFocusBringIntoViewPolicy::Nearest
                               ? ClampOffset(static_cast<std::int64_t>(offsets[scrollElement].x) + desiredX.Value(),
                                             minimumOffsets[scrollElement].x, maximumOffsets[scrollElement].x)
                               : ClampOffset(desiredX.Value(), minimumOffsets[scrollElement].x, maximumOffsets[scrollElement].x);
        const auto nextY = policy == UiFocusBringIntoViewPolicy::Nearest
                               ? ClampOffset(static_cast<std::int64_t>(offsets[scrollElement].y) + desiredY.Value(),
                                             minimumOffsets[scrollElement].y, maximumOffsets[scrollElement].y)
                               : ClampOffset(desiredY.Value(), minimumOffsets[scrollElement].y, maximumOffsets[scrollElement].y);
        if (nextX.HasError() || nextY.HasError())
            return Failure(UiErrors::LayoutClipInvalid);
        offsets[scrollElement] = {nextX.Value(), nextY.Value()};
        candidateScrolls[scrollIndexes[scrollElement]].offset = offsets[scrollElement];
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::RevealScrollContainer(const std::span<const UiLayoutRecord> records,
                                                                    const std::size_t pathIndex, const std::uint32_t target,
                                                                    const UiFocusBringIntoViewPolicy policy,
                                                                    const UiLogicalPoint innerTranslation) {
        const auto scrollElement = path[pathIndex];
        if (scrollIndexes[scrollElement] == NoIndex)
            return Result<void>::Success();
        const auto targetBeforeScroll = Translate(records[target].arrangement.hitTest, innerTranslation);
        if (targetBeforeScroll.HasError())
            return Result<void>::Failure(targetBeforeScroll.ErrorValue());
        const auto currentTranslation = OffsetDelta(offsets[scrollElement]);
        if (currentTranslation.HasError())
            return Result<void>::Failure(currentTranslation.ErrorValue());
        const auto visibleTarget = Translate(targetBeforeScroll.Value(), currentTranslation.Value());
        if (visibleTarget.HasError())
            return Result<void>::Failure(visibleTarget.ErrorValue());
        const auto &targetForPolicy = policy == UiFocusBringIntoViewPolicy::Nearest ? visibleTarget.Value() : targetBeforeScroll.Value();
        return SetRevealOffset(scrollElement, policy, targetForPolicy);
    }

    Result<void> UiLayoutClipEngine::Storage::ApplyBringIntoView(const std::span<const UiLayoutRecord> records,
                                                                 const UiFocusBringIntoViewRequest &request) {
        const auto target = FindRecord(records, request.target.element);
        if (target == NoIndex)
            return Failure(UiErrors::LayoutClipSourceStale);
        if (const auto pathResult = BuildRevealPath(target); pathResult.HasError())
            return pathResult;
        UiLogicalPoint innerTranslation{};
        for (std::size_t pathIndex = 1; pathIndex < path.size(); ++pathIndex) {
            const auto scrollElement = path[pathIndex];
            if (scrollIndexes[scrollElement] == NoIndex)
                continue;
            if (const auto reveal = RevealScrollContainer(records, pathIndex, target, request.policy, innerTranslation); reveal.HasError())
                return reveal;
            const auto delta = OffsetDelta(offsets[scrollElement]);
            if (delta.HasError())
                return Result<void>::Failure(delta.ErrorValue());
            const auto translated = Add(innerTranslation, delta.Value());
            if (translated.HasError())
                return Result<void>::Failure(translated.ErrorValue());
            innerTranslation = translated.Value();
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildParentProjection(const std::uint32_t index) {
        const auto parent = parents[index];
        if (parent == NoIndex)
            return Result<void>::Success();
        translations[index] = translations[parent];
        clipIndexes[index] = ownClipIndexes[parent] != NoIndex ? ownClipIndexes[parent] : clipIndexes[parent];
        if (scrollIndexes[parent] == NoIndex)
            return Result<void>::Success();
        const auto delta = OffsetDelta(offsets[parent]);
        if (delta.HasError())
            return Result<void>::Failure(delta.ErrorValue());
        const auto translated = Add(translations[index], delta.Value());
        if (translated.HasError())
            return Result<void>::Failure(translated.ErrorValue());
        translations[index] = translated.Value();
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildClipProjection(const std::span<const UiLayoutRecord> records,
                                                                  const std::span<const UiLayoutClipDescriptor> descriptors,
                                                                  const std::uint32_t index) {
        if (const auto overflow = descriptors[index].overflow;
            overflow != UiLayoutOverflowPolicy::Clip && overflow != UiLayoutOverflowPolicy::Scroll)
            return Result<void>::Success();
        if (candidateClips.size() == descriptor.clipCapacity)
            return Failure(UiErrors::CapacityExceeded);
        const auto clipRect = Translate(records[index].arrangement.contentBox, translations[index]);
        if (clipRect.HasError())
            return Result<void>::Failure(clipRect.ErrorValue());
        ownClipIndexes[index] = static_cast<std::uint32_t>(candidateClips.size());
        candidateClips.emplace_back(records[index].element, clipRect.Value(), clipIndexes[index]);
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildScrollProjection(const std::uint32_t index) {
        if (scrollIndexes[index] == NoIndex)
            return Result<void>::Success();
        auto &scroll = candidateScrolls[scrollIndexes[index]];
        const auto viewport = Translate(viewports[index], translations[index]);
        const auto content = Translate(contents[index], translations[index]);
        if (viewport.HasError() || content.HasError())
            return Failure(UiErrors::LayoutClipInvalid);
        scroll.viewport = viewport.Value();
        scroll.content = content.Value();
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildProjectionRecord(const std::span<const UiLayoutRecord> records,
                                                                    const std::span<const UiLayoutClipDescriptor> descriptors,
                                                                    const std::uint32_t index) {
        if (const auto parent = BuildParentProjection(index); parent.HasError())
            return parent;
        if (const auto clip = BuildClipProjection(records, descriptors, index); clip.HasError())
            return clip;
        if (const auto scroll = BuildScrollProjection(index); scroll.HasError())
            return scroll;
        candidateRecords.emplace_back(records[index].element, translations[index], clipIndexes[index], ownClipIndexes[index],
                                      scrollIndexes[index]);
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::BuildProjection(const std::span<const UiLayoutRecord> records,
                                                              const std::span<const UiLayoutClipDescriptor> descriptors) {
        candidateRecords.clear();
        for (std::uint32_t index = 0; index < records.size(); ++index) {
            if (const auto record = BuildProjectionRecord(records, descriptors, index); record.HasError())
                return record;
        }
        return Result<void>::Success();
    }

    Result<void> UiLayoutClipEngine::Storage::ValidateProjection(const std::size_t recordCount) const {
        if (candidateRecords.size() != recordCount)
            return Failure(UiErrors::LayoutClipInvalid);
        for (std::uint32_t index = 0; index < candidateClips.size(); ++index) {
            const auto &clip = candidateClips[index];
            if (!clip.IsValid() || (clip.parent != NoIndex && clip.parent >= index))
                return Failure(UiErrors::LayoutClipInvalid);
        }
        for (const auto &scroll : candidateScrolls) {
            if (!scroll.IsValid())
                return Failure(UiErrors::LayoutClipInvalid);
        }
        for (const auto &record : candidateRecords) {
            if (!record.IsValid() || (record.clip != NoIndex && record.clip >= candidateClips.size()) ||
                (record.ownClip != NoIndex && record.ownClip >= candidateClips.size()) ||
                (record.scroll != NoIndex && record.scroll >= candidateScrolls.size()))
                return Failure(UiErrors::LayoutClipInvalid);
        }
        return Result<void>::Success();
    }

    Result<std::shared_ptr<UiLayoutClipSnapshot::Storage>> UiLayoutClipEngine::Storage::Publish(const UiLayoutSnapshotDescriptor &source) {
        auto slot = TryAcquire();
        if (!slot)
            return Failure<std::shared_ptr<UiLayoutClipSnapshot::Storage>>(UiErrors::LayoutClipSnapshotStorageExhausted);
        slot->descriptor = {source.instance, source.canvas, source.document, source.sources, source.interaction};
        slot->records.resize(candidateRecords.size());
        std::ranges::copy(candidateRecords, slot->records.begin());
        slot->clips.resize(candidateClips.size());
        std::ranges::copy(candidateClips, slot->clips.begin());
        slot->scrolls.resize(candidateScrolls.size());
        std::ranges::copy(candidateScrolls, slot->scrolls.begin());
        slot->recordLookup.resize(slot->records.size());
        for (std::uint32_t index = 0; index < slot->records.size(); ++index)
            slot->recordLookup[index] = index;
        std::ranges::sort(slot->recordLookup, {}, [&records = slot->records](const std::uint32_t index) {
            return records[index].element;
        });
        ReleaseCurrent();
        current = slot;
        slot->leases.fetch_add(1);
        return Result<std::shared_ptr<UiLayoutClipSnapshot::Storage>>::Success(std::move(slot));
    }

    /** @copydoc UiLayoutClipEngine::Create */
    Result<UiLayoutClipEngine> UiLayoutClipEngine::Create(const UiLayoutClipEngineDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiLayoutClipEngine>(UiErrors::LayoutClipInvalid);
        try {
            return Result<UiLayoutClipEngine>::Success(UiLayoutClipEngine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiLayoutClipEngine>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiLayoutClipEngine::UiLayoutClipEngine */
    UiLayoutClipEngine::UiLayoutClipEngine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutClipEngine::~UiLayoutClipEngine */
    UiLayoutClipEngine::~UiLayoutClipEngine() {
        Shutdown();
    }

    /** @copydoc UiLayoutClipEngine::UiLayoutClipEngine */
    UiLayoutClipEngine::UiLayoutClipEngine(UiLayoutClipEngine &&other) noexcept = default;

    /** @copydoc UiLayoutClipEngine::operator= */
    UiLayoutClipEngine &UiLayoutClipEngine::operator=(UiLayoutClipEngine &&other) noexcept = default;

    /** @copydoc UiLayoutClipEngine::Update */
    Result<UiLayoutClipSnapshot> UiLayoutClipEngine::Update(const UiElementTree &tree, const UiLayoutSnapshot &layout,
                                                            const UiLayoutClipUpdateRequest &request) {
        if (!storage_ || storage_->lifecycle != UiLayoutClipEngineState::Active)
            return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipLifecycleUnavailable);
        const auto &source = layout.Descriptor();
        const auto records = layout.Records();
        if (const auto validation = storage_->ValidateUpdate(tree, source, records, request); validation.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(validation.ErrorValue());

        if (const auto topology = storage_->BuildParentIndex(tree, records); topology.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(topology.ErrorValue());
        if (const auto bounds = storage_->BuildScrollBounds(records, request.elements); bounds.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(bounds.ErrorValue());
        if (request.bringIntoView.has_value()) {
            if (const auto reveal = storage_->ApplyBringIntoView(records, *request.bringIntoView); reveal.HasError())
                return Result<UiLayoutClipSnapshot>::Failure(reveal.ErrorValue());
        }
        if (const auto projection = storage_->BuildProjection(records, request.elements); projection.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(projection.ErrorValue());
        if (const auto projection = storage_->ValidateProjection(records.size()); projection.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(projection.ErrorValue());

        auto published = storage_->Publish(source);
        if (published.HasError())
            return Result<UiLayoutClipSnapshot>::Failure(published.ErrorValue());
        return Result<UiLayoutClipSnapshot>::Success(UiLayoutClipSnapshot{std::move(published).Value()});
    }

    /** @copydoc UiLayoutClipEngine::BeginRetirement */
    Result<void> UiLayoutClipEngine::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiLayoutClipEngineState::Active)
            return Failure(UiErrors::LayoutClipLifecycleUnavailable);
        storage_->lifecycle = UiLayoutClipEngineState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiLayoutClipEngine::Shutdown */
    void UiLayoutClipEngine::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiLayoutClipEngineState::Stopped)
            return;
        storage_->lifecycle = UiLayoutClipEngineState::Stopped;
        storage_->recordLookup.clear();
        storage_->parents.clear();
        storage_->clipIndexes.clear();
        storage_->ownClipIndexes.clear();
        storage_->scrollIndexes.clear();
        storage_->path.clear();
        storage_->candidateRecords.clear();
        storage_->candidateClips.clear();
        storage_->candidateScrolls.clear();
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiLayoutClipEngine::State */
    UiLayoutClipEngineState UiLayoutClipEngine::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiLayoutClipEngineState::Stopped;
    }

    /** @copydoc UiLayoutClipEngine::IsDrained */
    bool UiLayoutClipEngine::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [&storage = *storage_](const auto &slot) {
            const auto leases = slot->leases.load();
            return leases == 0 || (slot == storage.current && leases == 1);
        });
    }
}  // namespace Horo::Runtime::Ui
