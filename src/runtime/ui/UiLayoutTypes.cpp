#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace Horo::Runtime::Ui {
    namespace {
        [[nodiscard]] bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
            return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
        }

        [[nodiscard]] bool IsLengthKind(const UiLengthKind kind) noexcept {
            using enum UiLengthKind;
            return kind == Auto || kind == Dip || kind == Percent;
        }

        [[nodiscard]] bool IsAlignment(const UiLayoutAlignment alignment) noexcept {
            using enum UiLayoutAlignment;
            return alignment == Start || alignment == Center || alignment == End || alignment == Stretch;
        }

        [[nodiscard]] bool IsPositioning(const UiLayoutPositioning positioning) noexcept {
            using enum UiLayoutPositioning;
            return positioning == Flow || positioning == Absolute;
        }

        [[nodiscard]] bool IsIntrinsicKind(const UiLayoutIntrinsicKind kind) noexcept {
            using enum UiLayoutIntrinsicKind;
            return kind == None || kind == Text || kind == Image;
        }

        [[nodiscard]] bool IsMinimumLength(const UiLength &length) noexcept {
            return length.kind != UiLengthKind::Auto && length.IsValid(false);
        }

        [[nodiscard]] bool IsMaximumLength(const UiLength &length) noexcept {
            return length.kind == UiLengthKind::Auto ? length.IsValid() : length.IsValid(false);
        }

        [[nodiscard]] bool IsValidSizeConstraints(const UiLayoutStyle &style) noexcept {
            return style.width.IsValid() && style.height.IsValid() && IsMinimumLength(style.minimumWidth) &&
                   IsMinimumLength(style.minimumHeight) && IsMaximumLength(style.maximumWidth) && IsMaximumLength(style.maximumHeight);
        }

        [[nodiscard]] bool IsValidBoxModel(const UiLayoutStyle &style) noexcept {
            return style.aspectRatio.IsValid() && style.margin.IsValid(true) && style.padding.IsValid() && style.border.IsValid() &&
                   style.offsets.IsValid(true);
        }

        [[nodiscard]] bool IsValidPlacement(const UiLayoutStyle &style) noexcept {
            return style.anchors.IsValid() && style.pivot.IsValid() && IsAlignment(style.horizontalAlignment) &&
                   IsAlignment(style.verticalAlignment) && IsPositioning(style.positioning);
        }
    }  // namespace

    namespace LayoutInternal {
        namespace {
            constexpr std::int64_t MinimumScalar = std::numeric_limits<std::int32_t>::min();
            constexpr std::int64_t MaximumScalar = std::numeric_limits<std::int32_t>::max();

            template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
                return Result<T>::Failure(MakeError(descriptor));
            }

            [[nodiscard]] Result<std::int32_t> RoundDivideEven(const std::int64_t numerator, const std::int64_t denominator) {
                if (denominator <= 0 || numerator < 0)
                    return Failure<std::int32_t>(UiErrors::LayoutInvalid);
                const auto quotient = numerator / denominator;
                const auto remainder = numerator % denominator;
                const auto half = denominator / 2;
                const bool roundUp = remainder > half || (remainder == half && denominator % 2 == 0 && quotient % 2 != 0);
                return CheckedCast(quotient + static_cast<std::int64_t>(roundUp));
            }
        }  // namespace

        [[nodiscard]] Result<std::int32_t> CheckedCast(const std::int64_t value) {
            if (value < MinimumScalar || value > MaximumScalar)
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(value));
        }

        [[nodiscard]] Result<std::int32_t> CheckedAdd(const std::int32_t left, const std::int32_t right) {
            return CheckedCast(static_cast<std::int64_t>(left) + right);
        }

        [[nodiscard]] Result<std::int32_t> CheckedSub(const std::int32_t left, const std::int32_t right) {
            return CheckedCast(static_cast<std::int64_t>(left) - right);
        }

        [[nodiscard]] Result<std::int32_t> MultiplyRatio(const std::int32_t value, const std::uint32_t numerator,
                                                         const std::uint32_t denominator) {
            if (value < 0 || denominator == 0)
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            if (numerator == 0)
                return Result<std::int32_t>::Success(0);
            const auto value64 = static_cast<std::int64_t>(value);
            const auto numerator64 = static_cast<std::int64_t>(numerator);
            if (value64 > std::numeric_limits<std::int64_t>::max() / numerator64)
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            return RoundDivideEven(value64 * numerator64, denominator);
        }

        [[nodiscard]] Result<std::int32_t> ResolveLength(const UiLength &length, const std::int32_t available, const bool autoValueIsZero) {
            using enum UiLengthKind;
            if (!length.IsValid() || available < 0)
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            switch (length.kind) {
                case Auto:
                    return autoValueIsZero ? Result<std::int32_t>::Success(0) : Failure<std::int32_t>(UiErrors::LayoutInvalid);
                case Dip:
                    return Result<std::int32_t>::Success(length.value);
                case Percent:
                    return MultiplyRatio(available, static_cast<std::uint32_t>(length.value), UiScalarUnitsPerDip);
            }
            return Failure<std::int32_t>(UiErrors::LayoutInvalid);
        }

        [[nodiscard]] Result<std::int32_t> ResolveOptionalMaximum(const UiLength &length, const std::int32_t available) {
            return length.kind == UiLengthKind::Auto ? Result<std::int32_t>::Success(available) : ResolveLength(length, available, true);
        }

        [[nodiscard]] Result<std::int32_t> AnchorLine(const std::int32_t origin, const std::int32_t extent,
                                                      const std::optional<UiScalar> anchor) {
            if (!anchor.has_value())
                return Result<std::int32_t>::Success(origin);
            auto offset = MultiplyRatio(extent, static_cast<std::uint32_t>(*anchor), UiScalarUnitsPerDip);
            return offset.HasError() ? offset : CheckedAdd(origin, offset.Value());
        }

        [[nodiscard]] Result<std::int32_t> PivotOffset(const std::int32_t extent, const std::int32_t pivot) {
            return MultiplyRatio(extent, static_cast<std::uint32_t>(pivot), UiScalarUnitsPerDip);
        }

        [[nodiscard]] Result<UiLogicalRect> MakeRect(const UiLogicalPoint origin, const UiLogicalExtent extent) {
            if (!extent.IsValid())
                return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
            const auto endX = CheckedAdd(origin.x, extent.width);
            const auto endY = CheckedAdd(origin.y, extent.height);
            return endX.HasError() || endY.HasError() ? Failure<UiLogicalRect>(UiErrors::LayoutInvalid)
                                                      : Result<UiLogicalRect>::Success({origin, extent});
        }

        [[nodiscard]] Result<UiLogicalRect> ExpandRect(const UiLogicalRect rect, const UiLayoutEdges edges) {
            const auto x = CheckedSub(rect.origin.x, edges.left);
            const auto y = CheckedSub(rect.origin.y, edges.top);
            const auto width = static_cast<std::int64_t>(rect.extent.width) + edges.left + edges.right;
            const auto height = static_cast<std::int64_t>(rect.extent.height) + edges.top + edges.bottom;
            if (x.HasError() || y.HasError())
                return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
            const auto checkedWidth = CheckedCast(std::max<std::int64_t>(0, width));
            const auto checkedHeight = CheckedCast(std::max<std::int64_t>(0, height));
            return checkedWidth.HasError() || checkedHeight.HasError()
                       ? Failure<UiLogicalRect>(UiErrors::LayoutInvalid)
                       : MakeRect({x.Value(), y.Value()}, {checkedWidth.Value(), checkedHeight.Value()});
        }

        [[nodiscard]] Result<UiLogicalRect> UnionRect(const UiLogicalRect first, const UiLogicalRect second) {
            const auto firstEndX = CheckedAdd(first.origin.x, first.extent.width);
            const auto firstEndY = CheckedAdd(first.origin.y, first.extent.height);
            const auto secondEndX = CheckedAdd(second.origin.x, second.extent.width);
            const auto secondEndY = CheckedAdd(second.origin.y, second.extent.height);
            if (firstEndX.HasError() || firstEndY.HasError() || secondEndX.HasError() || secondEndY.HasError())
                return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
            const auto originX = std::min(first.origin.x, second.origin.x);
            const auto originY = std::min(first.origin.y, second.origin.y);
            const auto endX = std::max(firstEndX.Value(), secondEndX.Value());
            const auto endY = std::max(firstEndY.Value(), secondEndY.Value());
            const auto width = CheckedCast(static_cast<std::int64_t>(endX) - originX);
            const auto height = CheckedCast(static_cast<std::int64_t>(endY) - originY);
            return width.HasError() || height.HasError() ? Failure<UiLogicalRect>(UiErrors::LayoutInvalid)
                                                         : MakeRect({originX, originY}, {width.Value(), height.Value()});
        }
    }  // namespace LayoutInternal

    /** @copydoc UiLogicalTransform::IsValid */
    bool UiLogicalTransform::IsValid() const noexcept {
        return std::ranges::all_of(values, [](const float value) {
            return std::isfinite(value);
        });
    }

    /** @copydoc UiLogicalExtent::IsValid */
    bool UiLogicalExtent::IsValid() const noexcept {
        return width >= 0 && height >= 0;
    }

    /** @copydoc UiLogicalRect::IsValid */
    bool UiLogicalRect::IsValid() const noexcept {
        return extent.IsValid();
    }

    /** @copydoc UiLayoutSourceRevisions::IsValid */
    bool UiLayoutSourceRevisions::IsValid() const noexcept {
        return document.IsValid() && tree.IsValid() && content.IsValid() && style.IsValid() && intrinsic.IsValid() && canvas.IsValid() &&
               policy.IsValid();
    }

    /** @copydoc UiLayoutConstraints::IsValid */
    bool UiLayoutConstraints::IsValid() const noexcept {
        return minimum.IsValid() && maximum.IsValid() && minimum.width <= maximum.width && minimum.height <= maximum.height;
    }

    /** @copydoc UiLength::IsValid */
    bool UiLength::IsValid(const bool allowNegative) const noexcept {
        using enum UiLengthKind;
        if (!IsLengthKind(kind))
            return false;
        switch (kind) {
            case Auto:
                return value == 0;
            case Dip:
                return allowNegative || value >= 0;
            case Percent:
                return value >= 0 && value <= UiScalarUnitsPerDip;
        }
        return false;
    }

    /** @copydoc UiAspectRatio::IsValid */
    bool UiAspectRatio::IsValid() const noexcept {
        return (width == 0 && height == 0) || (width > 0 && height > 0);
    }

    /** @copydoc UiLayoutEdges::IsValid */
    bool UiLayoutEdges::IsValid(const bool allowNegative) const noexcept {
        return allowNegative || (left >= 0 && top >= 0 && right >= 0 && bottom >= 0);
    }

    /** @copydoc UiLayoutAnchorAxis::IsValid */
    bool UiLayoutAnchorAxis::IsValid() const noexcept {
        const auto validAnchor = [](const std::optional<UiScalar> value) noexcept {
            return !value.has_value() || (value.value() >= 0 && value.value() <= UiScalarUnitsPerDip);
        };
        return validAnchor(start) && validAnchor(end) && IsAlignment(alignment);
    }

    /** @copydoc UiLayoutAnchors::IsValid */
    bool UiLayoutAnchors::IsValid() const noexcept {
        return horizontal.IsValid() && vertical.IsValid();
    }

    /** @copydoc UiLayoutPivot::IsValid */
    bool UiLayoutPivot::IsValid() const noexcept {
        return x >= 0 && x <= UiScalarUnitsPerDip && y >= 0 && y <= UiScalarUnitsPerDip;
    }

    /** @copydoc UiLayoutStyle::IsValid */
    bool UiLayoutStyle::IsValid() const noexcept {
        return IsValidSizeConstraints(*this) && IsValidBoxModel(*this) && IsValidPlacement(*this);
    }

    /** @copydoc UiLayoutIntrinsicSource::IsValid */
    bool UiLayoutIntrinsicSource::IsValid() const noexcept {
        return IsIntrinsicKind(kind) && fallback.IsValid() && (kind != UiLayoutIntrinsicKind::None || !required);
    }

    /** @copydoc UiLayoutElementDescriptor::IsValid */
    bool UiLayoutElementDescriptor::IsValid() const noexcept {
        return element.IsValid() && style.IsValid() && intrinsic.IsValid();
    }

    /** @copydoc UiLayoutIntrinsicMeasurement::IsValid */
    bool UiLayoutIntrinsicMeasurement::IsValid() const noexcept {
        return preferred.IsValid() && baseline >= NoUiBaseline;
    }

    /** @copydoc UiLayoutMeasurement::IsValid */
    bool UiLayoutMeasurement::IsValid(const UiLayoutConstraints &constraints) const noexcept {
        return constraints.IsValid() && desired.IsValid() && desired.width >= constraints.minimum.width &&
               desired.width <= constraints.maximum.width && desired.height >= constraints.minimum.height &&
               desired.height <= constraints.maximum.height && constraintResult <= UiLayoutConstraintResult::Unsatisfiable &&
               baseline >= NoUiBaseline;
    }

    /** @copydoc UiLayoutArrangement::IsValid */
    bool UiLayoutArrangement::IsValid() const noexcept {
        return marginBox.IsValid() && borderBox.IsValid() && paddingBox.IsValid() && contentBox.IsValid() && overflow.IsValid() &&
               hitTest.IsValid() && baseline >= NoUiBaseline;
    }

    /** @copydoc UiLayoutEngineDescriptor::IsValid */
    bool UiLayoutEngineDescriptor::IsValid() const noexcept {
        return SameOwner(instance, canvas) && document.IsValid() && elementCapacity > 0 && elementCapacity <= MaximumUiTreeElements &&
               invalidationCapacity > 0 && invalidationCapacity <= MaximumUiStructuralCommands && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiLayoutSnapshotsInFlight && initialInteractionRevision.IsValid();
    }
}  // namespace Horo::Runtime::Ui
