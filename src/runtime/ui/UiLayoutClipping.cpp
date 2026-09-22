#include "Horo/Runtime/Ui/UiLayoutClipping.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::uint32_t NoIndex = std::numeric_limits<std::uint32_t>::max();
        constexpr std::int64_t MinimumScalar = std::numeric_limits<std::int32_t>::min();
        constexpr std::int64_t MaximumScalar = std::numeric_limits<std::int32_t>::max();

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const UiLayoutOverflowPolicy policy) noexcept {
            return static_cast<std::uint8_t>(policy) < static_cast<std::uint8_t>(UiLayoutOverflowPolicy::Count);
        }

        [[nodiscard]] bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
            return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
        }

        [[nodiscard]] Result<std::int32_t> CheckedCast(const std::int64_t value) {
            if (value < MinimumScalar || value > MaximumScalar)
                return Failure<std::int32_t>(UiErrors::LayoutClipInvalid);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(value));
        }

        [[nodiscard]] Result<UiLogicalPoint> Add(const UiLogicalPoint first, const UiLogicalPoint second) {
            const auto x = CheckedCast(static_cast<std::int64_t>(first.x) + second.x);
            const auto y = CheckedCast(static_cast<std::int64_t>(first.y) + second.y);
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalPoint>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalPoint>::Success({x.Value(), y.Value()});
        }

        [[nodiscard]] Result<UiLogicalPoint> Negate(const UiLogicalPoint value) {
            const auto x = CheckedCast(-static_cast<std::int64_t>(value.x));
            const auto y = CheckedCast(-static_cast<std::int64_t>(value.y));
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalPoint>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalPoint>::Success({x.Value(), y.Value()});
        }

        [[nodiscard]] std::int64_t Right(const UiLogicalRect &rect) noexcept {
            return static_cast<std::int64_t>(rect.origin.x) + rect.extent.width;
        }

        [[nodiscard]] std::int64_t Bottom(const UiLogicalRect &rect) noexcept {
            return static_cast<std::int64_t>(rect.origin.y) + rect.extent.height;
        }

        [[nodiscard]] Result<UiLogicalRect> Translate(const UiLogicalRect rect, const UiLogicalPoint translation) {
            if (!rect.IsValid())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            const auto x = CheckedCast(static_cast<std::int64_t>(rect.origin.x) + translation.x);
            const auto y = CheckedCast(static_cast<std::int64_t>(rect.origin.y) + translation.y);
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalRect>::Success({{x.Value(), y.Value()}, rect.extent});
        }

        [[nodiscard]] Result<UiLogicalRect> Union(const UiLogicalRect first, const UiLogicalRect second) {
            if (!first.IsValid() || !second.IsValid())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            const auto left = std::min<std::int64_t>(first.origin.x, second.origin.x);
            const auto top = std::min<std::int64_t>(first.origin.y, second.origin.y);
            const auto right = std::max(Right(first), Right(second));
            const auto bottom = std::max(Bottom(first), Bottom(second));
            const auto width = CheckedCast(right - left);
            const auto height = CheckedCast(bottom - top);
            const auto originX = CheckedCast(left);
            const auto originY = CheckedCast(top);
            if (width.HasError() || height.HasError() || originX.HasError() || originY.HasError())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalRect>::Success({{originX.Value(), originY.Value()}, {width.Value(), height.Value()}});
        }

        [[nodiscard]] Result<std::int32_t> ClampOffset(const std::int64_t value, const std::int32_t minimum, const std::int32_t maximum) {
            if (minimum > maximum)
                return Failure<std::int32_t>(UiErrors::LayoutClipInvalid);
            return Result<std::int32_t>::Success(
                static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum))));
        }

        [[nodiscard]] Result<std::int64_t> RoundDivideByTwo(const std::int64_t numerator) {
            const auto quotient = numerator / 2;
            const auto remainder = numerator % 2;
            if (remainder == 0)
                return Result<std::int64_t>::Success(quotient);
            if (remainder != 1 && remainder != -1)
                return Failure<std::int64_t>(UiErrors::LayoutClipInvalid);
            if ((quotient % 2) != 0)
                return Result<std::int64_t>::Success(quotient + (numerator > 0 ? 1 : -1));
            return Result<std::int64_t>::Success(quotient);
        }

        [[nodiscard]] Result<std::int64_t> DesiredOffset(const UiFocusBringIntoViewPolicy policy, const UiLogicalRect target,
                                                         const UiLogicalRect viewport, const bool horizontal) {
            const auto targetStart = horizontal ? target.origin.x : target.origin.y;
            const auto targetEnd = horizontal ? Right(target) : Bottom(target);
            const auto viewportStart = horizontal ? viewport.origin.x : viewport.origin.y;
            const auto viewportEnd = horizontal ? Right(viewport) : Bottom(viewport);
            switch (policy) {
                case UiFocusBringIntoViewPolicy::Nearest: {
                    const auto before = targetStart - viewportStart;
                    const auto after = targetEnd - viewportEnd;
                    if (before < 0 && after > 0)
                        return Result<std::int64_t>::Success(-before <= after ? before : after);
                    if (before < 0)
                        return Result<std::int64_t>::Success(before);
                    if (after > 0)
                        return Result<std::int64_t>::Success(after);
                    return Result<std::int64_t>::Success(0);
                }
                case UiFocusBringIntoViewPolicy::Start:
                    return Result<std::int64_t>::Success(targetStart - viewportStart);
                case UiFocusBringIntoViewPolicy::Center: {
                    const auto targetExtent = static_cast<std::int64_t>(horizontal ? target.extent.width : target.extent.height);
                    const auto viewportExtent = static_cast<std::int64_t>(horizontal ? viewport.extent.width : viewport.extent.height);
                    const auto targetCenterTwice = 2 * static_cast<std::int64_t>(targetStart) + targetExtent;
                    const auto viewportCenterTwice = 2 * static_cast<std::int64_t>(viewportStart) + viewportExtent;
                    return RoundDivideByTwo(targetCenterTwice - viewportCenterTwice);
                }
                case UiFocusBringIntoViewPolicy::End:
                    return Result<std::int64_t>::Success(targetEnd - viewportEnd);
                case UiFocusBringIntoViewPolicy::None:
                case UiFocusBringIntoViewPolicy::Count:
                    return Failure<std::int64_t>(UiErrors::LayoutClipInvalid);
            }
            return Failure<std::int64_t>(UiErrors::LayoutClipInvalid);
        }

        [[nodiscard]] Result<UiLogicalPoint> OffsetDelta(const UiLogicalPoint value) {
            return Negate(value);
        }
    }  // namespace

    /** @copydoc UiLayoutClipDescriptor::IsValid */
    bool UiLayoutClipDescriptor::IsValid() const noexcept {
        if (!element.IsValid() || !IsKnown(overflow))
            return false;
        return overflow == UiLayoutOverflowPolicy::Scroll || scrollOffset == UiLogicalPoint{};
    }

    /** @copydoc UiLayoutClipNode::IsValid */
    bool UiLayoutClipNode::IsValid() const noexcept {
        return element.IsValid() && rect.IsValid();
    }

    /** @copydoc UiLayoutScrollRecord::IsValid */
    bool UiLayoutScrollRecord::IsValid() const noexcept {
        return element.IsValid() && viewport.IsValid() && content.IsValid() && minimumOffset.x <= maximumOffset.x &&
               minimumOffset.y <= maximumOffset.y && content.extent.width >= viewport.extent.width &&
               content.extent.height >= viewport.extent.height && minimumOffset.x <= offset.x && offset.x <= maximumOffset.x &&
               minimumOffset.y <= offset.y && offset.y <= maximumOffset.y;
    }

    /** @copydoc UiLayoutClipRecord::IsValid */
    bool UiLayoutClipRecord::IsValid() const noexcept {
        return element.IsValid();
    }

    /** @copydoc UiLayoutClipSnapshotDescriptor::IsValid */
    bool UiLayoutClipSnapshotDescriptor::IsValid() const noexcept {
        return SameOwner(instance, canvas) && document.IsValid() && sources.IsValid() && interaction.IsValid();
    }

    /** @copydoc UiLayoutClipEngineDescriptor::IsValid */
    bool UiLayoutClipEngineDescriptor::IsValid() const noexcept {
        return SameOwner(instance, canvas) && document.IsValid() && elementCapacity > 0 && elementCapacity <= MaximumUiTreeElements &&
               clipCapacity > 0 && clipCapacity <= MaximumUiLayoutClipNodes && scrollCapacity > 0 &&
               scrollCapacity <= MaximumUiLayoutScrollContainers && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiLayoutClipSnapshotsInFlight;
    }

    struct UiLayoutClipSnapshot::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiLayoutClipSnapshotDescriptor descriptor;
        std::vector<UiLayoutClipRecord> records;
        std::vector<UiLayoutClipNode> clips;
        std::vector<UiLayoutScrollRecord> scrolls;
        std::vector<std::uint32_t> recordLookup;

        explicit Storage(const UiLayoutClipEngineDescriptor &source) {
            records.reserve(source.elementCapacity);
            clips.reserve(source.clipCapacity);
            scrolls.reserve(source.scrollCapacity);
            recordLookup.reserve(source.elementCapacity);
        }
    };

    struct UiLayoutClipEngine::Storage final {
        UiLayoutClipEngineDescriptor descriptor;
        UiLayoutClipEngineState lifecycle{UiLayoutClipEngineState::Active};
        std::vector<std::shared_ptr<UiLayoutClipSnapshot::Storage>> slots;
        std::shared_ptr<UiLayoutClipSnapshot::Storage> current;
        std::size_t nextSlot{};

        std::vector<std::uint32_t> recordLookup;
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> clipIndexes;
        std::vector<std::uint32_t> ownClipIndexes;
        std::vector<std::uint32_t> scrollIndexes;
        std::vector<std::uint32_t> path;
        std::vector<UiLogicalPoint> translations;
        std::vector<UiLogicalPoint> offsets;
        std::vector<UiLogicalPoint> minimumOffsets;
        std::vector<UiLogicalPoint> maximumOffsets;
        std::vector<UiLogicalRect> viewports;
        std::vector<UiLogicalRect> contents;
        std::vector<UiLayoutClipRecord> candidateRecords;
        std::vector<UiLayoutClipNode> candidateClips;
        std::vector<UiLayoutScrollRecord> candidateScrolls;

        explicit Storage(const UiLayoutClipEngineDescriptor &source) : descriptor(source) {
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

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        ~Storage() {
            ReleaseCurrent();
        }

        void ReleaseCurrent() noexcept {
            if (current) {
                current->leases.fetch_sub(1);
                current.reset();
            }
        }

        [[nodiscard]] std::shared_ptr<UiLayoutClipSnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                    continue;
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        [[nodiscard]] std::uint32_t FindRecord(const std::span<const UiLayoutRecord> records,
                                               const UiElementHandle element) const noexcept {
            const auto found = std::ranges::lower_bound(recordLookup, element, {}, [&records](const std::uint32_t index) {
                return records[index].element;
            });
            return found != recordLookup.end() && records[*found].element == element ? *found : NoIndex;
        }

        [[nodiscard]] Result<void> BuildParentIndex(const UiElementTree &tree, const std::span<const UiLayoutRecord> records) {
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

        [[nodiscard]] Result<void> BuildScrollBounds(const std::span<const UiLayoutRecord> records,
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
                candidateScrolls.push_back({records[index].element, viewports[index], contents[index], offsets[index],
                                            minimumOffsets[index], maximumOffsets[index]});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyBringIntoView(const std::span<const UiLayoutRecord> records,
                                                      const UiFocusBringIntoViewRequest &request) {
            const auto target = FindRecord(records, request.target.element);
            if (target == NoIndex)
                return Failure(UiErrors::LayoutClipSourceStale);
            path.clear();
            for (std::uint32_t current = target; current != NoIndex; current = parents[current]) {
                if (path.size() == descriptor.elementCapacity)
                    return Failure(UiErrors::LayoutClipInvalid);
                path.push_back(current);
            }

            for (std::size_t pathIndex = 1; pathIndex < path.size(); ++pathIndex) {
                const auto scrollElement = path[pathIndex];
                if (scrollIndexes[scrollElement] == NoIndex)
                    continue;

                UiLogicalPoint innerTranslation{};
                for (std::size_t innerIndex = 1; innerIndex < pathIndex; ++innerIndex) {
                    const auto innerElement = path[innerIndex];
                    if (scrollIndexes[innerElement] == NoIndex)
                        continue;
                    const auto delta = OffsetDelta(offsets[innerElement]);
                    if (delta.HasError())
                        return Result<void>::Failure(delta.ErrorValue());
                    const auto translated = Add(innerTranslation, delta.Value());
                    if (translated.HasError())
                        return Result<void>::Failure(translated.ErrorValue());
                    innerTranslation = translated.Value();
                }
                const auto targetBeforeScroll = Translate(records[target].arrangement.hitTest, innerTranslation);
                if (targetBeforeScroll.HasError())
                    return Result<void>::Failure(targetBeforeScroll.ErrorValue());
                const auto currentTranslation = OffsetDelta(offsets[scrollElement]);
                if (currentTranslation.HasError())
                    return Result<void>::Failure(currentTranslation.ErrorValue());
                const auto visibleTarget = Translate(targetBeforeScroll.Value(), currentTranslation.Value());
                if (visibleTarget.HasError())
                    return Result<void>::Failure(visibleTarget.ErrorValue());

                const auto &targetForPolicy =
                    request.policy == UiFocusBringIntoViewPolicy::Nearest ? visibleTarget.Value() : targetBeforeScroll.Value();
                const auto desiredX = DesiredOffset(request.policy, targetForPolicy, viewports[scrollElement], true);
                const auto desiredY = DesiredOffset(request.policy, targetForPolicy, viewports[scrollElement], false);
                if (desiredX.HasError() || desiredY.HasError())
                    return Failure(UiErrors::LayoutClipInvalid);
                const auto nextX = request.policy == UiFocusBringIntoViewPolicy::Nearest
                                       ? ClampOffset(static_cast<std::int64_t>(offsets[scrollElement].x) + desiredX.Value(),
                                                     minimumOffsets[scrollElement].x, maximumOffsets[scrollElement].x)
                                       : ClampOffset(desiredX.Value(), minimumOffsets[scrollElement].x, maximumOffsets[scrollElement].x);
                const auto nextY = request.policy == UiFocusBringIntoViewPolicy::Nearest
                                       ? ClampOffset(static_cast<std::int64_t>(offsets[scrollElement].y) + desiredY.Value(),
                                                     minimumOffsets[scrollElement].y, maximumOffsets[scrollElement].y)
                                       : ClampOffset(desiredY.Value(), minimumOffsets[scrollElement].y, maximumOffsets[scrollElement].y);
                if (nextX.HasError() || nextY.HasError())
                    return Failure(UiErrors::LayoutClipInvalid);
                offsets[scrollElement] = {nextX.Value(), nextY.Value()};
                candidateScrolls[scrollIndexes[scrollElement]].offset = offsets[scrollElement];
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildProjection(const std::span<const UiLayoutRecord> records,
                                                   const std::span<const UiLayoutClipDescriptor> descriptors) {
            candidateRecords.clear();
            for (std::uint32_t index = 0; index < records.size(); ++index) {
                const auto parent = parents[index];
                if (parent != NoIndex) {
                    translations[index] = translations[parent];
                    if (scrollIndexes[parent] != NoIndex) {
                        const auto delta = OffsetDelta(offsets[parent]);
                        if (delta.HasError())
                            return Result<void>::Failure(delta.ErrorValue());
                        const auto translated = Add(translations[index], delta.Value());
                        if (translated.HasError())
                            return Result<void>::Failure(translated.ErrorValue());
                        translations[index] = translated.Value();
                    }
                    clipIndexes[index] = ownClipIndexes[parent] != NoIndex ? ownClipIndexes[parent] : clipIndexes[parent];
                }

                if (descriptors[index].overflow == UiLayoutOverflowPolicy::Clip ||
                    descriptors[index].overflow == UiLayoutOverflowPolicy::Scroll) {
                    if (candidateClips.size() == descriptor.clipCapacity)
                        return Failure(UiErrors::CapacityExceeded);
                    const auto clipRect = Translate(records[index].arrangement.contentBox, translations[index]);
                    if (clipRect.HasError())
                        return Result<void>::Failure(clipRect.ErrorValue());
                    ownClipIndexes[index] = static_cast<std::uint32_t>(candidateClips.size());
                    candidateClips.push_back({records[index].element, clipRect.Value(), clipIndexes[index]});
                }
                if (scrollIndexes[index] != NoIndex) {
                    auto &scroll = candidateScrolls[scrollIndexes[index]];
                    const auto viewport = Translate(viewports[index], translations[index]);
                    const auto content = Translate(contents[index], translations[index]);
                    if (viewport.HasError() || content.HasError())
                        return Failure(UiErrors::LayoutClipInvalid);
                    scroll.viewport = viewport.Value();
                    scroll.content = content.Value();
                }
                candidateRecords.push_back(
                    {records[index].element, translations[index], clipIndexes[index], ownClipIndexes[index], scrollIndexes[index]});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateProjection(const std::size_t recordCount) const {
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

        [[nodiscard]] Result<std::shared_ptr<UiLayoutClipSnapshot::Storage>> Publish(const UiLayoutSnapshotDescriptor &source) {
            const auto slot = TryAcquire();
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
    };

    /** @copydoc UiLayoutClipSnapshot::UiLayoutClipSnapshot */
    UiLayoutClipSnapshot::UiLayoutClipSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiLayoutClipSnapshot::~UiLayoutClipSnapshot */
    UiLayoutClipSnapshot::~UiLayoutClipSnapshot() {
        Release();
    }

    /** @copydoc UiLayoutClipSnapshot::UiLayoutClipSnapshot */
    UiLayoutClipSnapshot::UiLayoutClipSnapshot(const UiLayoutClipSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiLayoutClipSnapshot::operator= */
    UiLayoutClipSnapshot &UiLayoutClipSnapshot::operator=(const UiLayoutClipSnapshot &other) noexcept {
        if (this != &other) {
            Release();
            storage_ = other.storage_;
            Retain();
        }
        return *this;
    }

    /** @copydoc UiLayoutClipSnapshot::UiLayoutClipSnapshot */
    UiLayoutClipSnapshot::UiLayoutClipSnapshot(UiLayoutClipSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiLayoutClipSnapshot::operator= */
    UiLayoutClipSnapshot &UiLayoutClipSnapshot::operator=(UiLayoutClipSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiLayoutClipSnapshot::Descriptor */
    const UiLayoutClipSnapshotDescriptor &UiLayoutClipSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiLayoutClipSnapshot::Records */
    std::span<const UiLayoutClipRecord> UiLayoutClipSnapshot::Records() const noexcept {
        return storage_->records;
    }

    /** @copydoc UiLayoutClipSnapshot::Clips */
    std::span<const UiLayoutClipNode> UiLayoutClipSnapshot::Clips() const noexcept {
        return storage_->clips;
    }

    /** @copydoc UiLayoutClipSnapshot::Scrolls */
    std::span<const UiLayoutScrollRecord> UiLayoutClipSnapshot::Scrolls() const noexcept {
        return storage_->scrolls;
    }

    /** @copydoc UiLayoutClipSnapshot::Get */
    Result<UiLayoutClipRecord> UiLayoutClipSnapshot::Get(const UiElementHandle element) const {
        if (!storage_ || !element.IsValid())
            return Failure<UiLayoutClipRecord>(UiErrors::LayoutClipInvalid);
        const auto found = std::ranges::lower_bound(storage_->recordLookup, element, {}, [this](const std::uint32_t index) {
            return storage_->records[index].element;
        });
        if (found == storage_->recordLookup.end() || storage_->records[*found].element != element)
            return Failure<UiLayoutClipRecord>(UiErrors::HandleStale);
        return Result<UiLayoutClipRecord>::Success(storage_->records[*found]);
    }

    /** @brief Retains the immutable storage slot while a copy remains live. */
    void UiLayoutClipSnapshot::Retain() const noexcept {
        if (storage_)
            storage_->leases.fetch_add(1);
    }

    /** @brief Releases the immutable storage slot after the final copy retires. */
    void UiLayoutClipSnapshot::Release() noexcept {
        if (storage_)
            storage_->leases.fetch_sub(1);
        storage_.reset();
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
        if (!source.sources.IsValid() || !source.interaction.IsValid() || records.empty() ||
            records.size() > storage_->descriptor.elementCapacity || request.elements.size() != records.size())
            return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipInvalid);
        if (source.instance != storage_->descriptor.instance || source.canvas != storage_->descriptor.canvas ||
            source.document != storage_->descriptor.document || tree.State() != UiElementTreeState::Active ||
            tree.Instance() != storage_->descriptor.instance || tree.Canvas() != storage_->descriptor.canvas ||
            tree.SourceDocument() != storage_->descriptor.document || tree.SourceDocumentRevision() != source.sources.document ||
            tree.Revision() != source.sources.tree)
            return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipSourceStale);

        for (std::uint32_t index = 0; index < records.size(); ++index) {
            if (!request.elements[index].IsValid() || request.elements[index].element != records[index].element)
                return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipInvalid);
        }
        if (request.bringIntoView.has_value()) {
            const auto &bring = *request.bringIntoView;
            if (!bring.IsValid())
                return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipInvalid);
            if (bring.owner.instance != source.instance || bring.owner.canvas != source.canvas || bring.owner.document != source.document ||
                bring.owner.documentRevision != source.sources.document || bring.owner.treeRevision != source.sources.tree ||
                bring.owner.interaction != source.interaction)
                return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipSourceStale);
            const auto target = tree.Get(bring.target.element);
            if (target.HasError())
                return Result<UiLayoutClipSnapshot>::Failure(target.ErrorValue());
            if (target.Value().id != bring.target.id)
                return Failure<UiLayoutClipSnapshot>(UiErrors::LayoutClipSourceStale);
        }

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
