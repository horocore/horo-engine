#include "UiLayoutClippingInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace UiLayoutClippingInternal {
        namespace {
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
        }  // namespace

        bool IsKnown(const UiLayoutOverflowPolicy policy) noexcept {
            return static_cast<std::uint8_t>(policy) < static_cast<std::uint8_t>(UiLayoutOverflowPolicy::Count);
        }

        bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
            return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
        }

        Result<std::int32_t> CheckedCast(const std::int64_t value) {
            if (value < MinimumScalar || value > MaximumScalar)
                return Failure<std::int32_t>(UiErrors::LayoutClipInvalid);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(value));
        }

        Result<UiLogicalPoint> Add(const UiLogicalPoint first, const UiLogicalPoint second) {
            const auto x = CheckedCast(static_cast<std::int64_t>(first.x) + second.x);
            const auto y = CheckedCast(static_cast<std::int64_t>(first.y) + second.y);
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalPoint>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalPoint>::Success({x.Value(), y.Value()});
        }

        Result<UiLogicalPoint> Negate(const UiLogicalPoint value) {
            const auto x = CheckedCast(-static_cast<std::int64_t>(value.x));
            const auto y = CheckedCast(-static_cast<std::int64_t>(value.y));
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalPoint>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalPoint>::Success({x.Value(), y.Value()});
        }

        std::int64_t Right(const UiLogicalRect &rect) noexcept {
            return static_cast<std::int64_t>(rect.origin.x) + rect.extent.width;
        }

        std::int64_t Bottom(const UiLogicalRect &rect) noexcept {
            return static_cast<std::int64_t>(rect.origin.y) + rect.extent.height;
        }

        Result<UiLogicalRect> Translate(const UiLogicalRect rect, const UiLogicalPoint translation) {
            if (!rect.IsValid())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            const auto x = CheckedCast(static_cast<std::int64_t>(rect.origin.x) + translation.x);
            const auto y = CheckedCast(static_cast<std::int64_t>(rect.origin.y) + translation.y);
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalRect>(UiErrors::LayoutClipInvalid);
            return Result<UiLogicalRect>::Success({{x.Value(), y.Value()}, rect.extent});
        }

        Result<UiLogicalRect> Union(const UiLogicalRect first, const UiLogicalRect second) {
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

        Result<std::int32_t> ClampOffset(const std::int64_t value, const std::int32_t minimum, const std::int32_t maximum) {
            if (minimum > maximum)
                return Failure<std::int32_t>(UiErrors::LayoutClipInvalid);
            return Result<std::int32_t>::Success(
                static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum))));
        }

        Result<std::int64_t> DesiredOffset(const UiFocusBringIntoViewPolicy policy, const UiLogicalRect target,
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

        Result<UiLogicalPoint> OffsetDelta(const UiLogicalPoint value) {
            return Negate(value);
        }
    }  // namespace UiLayoutClippingInternal

    /** @copydoc UiLayoutClipDescriptor::IsValid */
    bool UiLayoutClipDescriptor::IsValid() const noexcept {
        if (!element.IsValid() || !UiLayoutClippingInternal::IsKnown(overflow))
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
        return UiLayoutClippingInternal::SameOwner(instance, canvas) && document.IsValid() && sources.IsValid() && interaction.IsValid();
    }

    /** @copydoc UiLayoutClipEngineDescriptor::IsValid */
    bool UiLayoutClipEngineDescriptor::IsValid() const noexcept {
        return UiLayoutClippingInternal::SameOwner(instance, canvas) && document.IsValid() && elementCapacity > 0 &&
               elementCapacity <= MaximumUiTreeElements && clipCapacity > 0 && clipCapacity <= MaximumUiLayoutClipNodes &&
               scrollCapacity > 0 && scrollCapacity <= MaximumUiLayoutScrollContainers && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiLayoutClipSnapshotsInFlight;
    }

    UiLayoutClipSnapshot::Storage::Storage(const UiLayoutClipEngineDescriptor &source) {
        records.reserve(source.elementCapacity);
        clips.reserve(source.clipCapacity);
        scrolls.reserve(source.scrollCapacity);
        recordLookup.reserve(source.elementCapacity);
    }

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
            if (other.storage_)
                other.storage_->leases.fetch_add(1);
            Release();
            storage_ = other.storage_;
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
        static const UiLayoutClipSnapshotDescriptor empty{};
        return storage_ ? storage_->descriptor : empty;
    }

    /** @copydoc UiLayoutClipSnapshot::Records */
    std::span<const UiLayoutClipRecord> UiLayoutClipSnapshot::Records() const noexcept {
        return storage_ ? std::span<const UiLayoutClipRecord>{storage_->records} : std::span<const UiLayoutClipRecord>{};
    }

    /** @copydoc UiLayoutClipSnapshot::Clips */
    std::span<const UiLayoutClipNode> UiLayoutClipSnapshot::Clips() const noexcept {
        return storage_ ? std::span<const UiLayoutClipNode>{storage_->clips} : std::span<const UiLayoutClipNode>{};
    }

    /** @copydoc UiLayoutClipSnapshot::Scrolls */
    std::span<const UiLayoutScrollRecord> UiLayoutClipSnapshot::Scrolls() const noexcept {
        return storage_ ? std::span<const UiLayoutScrollRecord>{storage_->scrolls} : std::span<const UiLayoutScrollRecord>{};
    }

    /** @copydoc UiLayoutClipSnapshot::Get */
    Result<UiLayoutClipRecord> UiLayoutClipSnapshot::Get(const UiElementHandle element) const {
        if (!storage_ || !element.IsValid())
            return UiLayoutClippingInternal::Failure<UiLayoutClipRecord>(UiErrors::LayoutClipInvalid);
        const auto found = std::ranges::lower_bound(storage_->recordLookup, element, {}, [this](const std::uint32_t index) {
            return storage_->records[index].element;
        });
        if (found == storage_->recordLookup.end() || storage_->records[*found].element != element)
            return UiLayoutClippingInternal::Failure<UiLayoutClipRecord>(UiErrors::HandleStale);
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
}  // namespace Horo::Runtime::Ui
