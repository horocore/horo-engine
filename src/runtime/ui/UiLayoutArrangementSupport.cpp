#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiLayoutArrangementInternal.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <memory>

namespace Horo::Runtime::Ui::LayoutInternal {
    [[nodiscard]] Result<std::int32_t> RoundDivideEven(const std::int64_t numerator, const std::uint32_t denominator) {
        if (denominator == 0 || numerator < 0)
            return Failure<std::int32_t>(UiErrors::LayoutInvalid);
        const auto denominator64 = static_cast<std::int64_t>(denominator);
        const auto quotient = numerator / denominator64;
        const auto remainder = numerator % denominator64;
        const auto half = denominator64 / 2;
        const bool roundUp = remainder > half || (remainder == half && denominator % 2 == 0 && quotient % 2 != 0);
        return CheckedCast(quotient + static_cast<std::int64_t>(roundUp));
    }

    [[nodiscard]] Result<std::int32_t> DistributedOffset(const std::int32_t total, const std::uint32_t numerator,
                                                         const std::uint32_t denominator) {
        return RoundDivideEven(static_cast<std::int64_t>(total) * numerator, denominator);
    }

    [[nodiscard]] Result<std::int32_t> DistributedDelta(const std::int32_t total, const std::uint32_t start, const std::uint32_t end,
                                                        const std::uint32_t denominator) {
        const auto startOffset = DistributedOffset(total, start, denominator);
        const auto endOffset = DistributedOffset(total, end, denominator);
        if (startOffset.HasError() || endOffset.HasError())
            return Failure<std::int32_t>(UiErrors::LayoutInvalid);
        return CheckedSub(endOffset.Value(), startOffset.Value());
    }

    /** @brief Computes floor(value * numerator / denominator) without overflowing the intermediate product.
     * @details The weighted allocation passes a numerator no greater than the denominator, so the quotient is no greater
     * than value. The binary long-division form keeps the remainder below the denominator at every step.
     */
    [[nodiscard]] std::uint64_t SafeWeightedShare(const std::uint64_t value, const std::uint64_t numerator,
                                                  const std::uint64_t denominator) noexcept {
        if (value == 0 || numerator == 0)
            return 0;
        if (value <= (~std::uint64_t{} / numerator))
            return value * numerator / denominator;
        std::uint64_t quotient{};
        std::uint64_t remainder{};
        for (int bit = 63; bit >= 0; --bit) {
            const auto half = denominator / 2 + denominator % 2;
            const bool doubledCarries = remainder >= half;
            auto doubled = doubledCarries ? remainder - (denominator - remainder) : remainder * 2;
            std::uint64_t carry = doubledCarries ? 1 : 0;
            if ((value & (std::uint64_t{1} << bit)) != 0) {
                const auto complement = denominator - numerator;
                if (doubled >= complement) {
                    doubled -= complement;
                    ++carry;
                } else {
                    doubled += numerator;
                }
            }
            quotient = quotient * 2 + carry;
            remainder = doubled;
        }
        return quotient;
    }

    namespace {
        [[nodiscard]] bool IsExplicit(const UiLength &length) noexcept {
            return length.kind != UiLengthKind::Auto;
        }

        [[nodiscard]] bool HasAnchor(const UiLayoutAnchorAxis &axis) noexcept {
            return axis.start.has_value() || axis.end.has_value();
        }

        struct AnchorLines final {
            std::int32_t start{};
            std::int32_t end{};
            bool hasStart{};
            bool hasEnd{};
        };

        [[nodiscard]] Result<AnchorLines> ResolveAnchorLines(const AxisPlacementRequest &request) {
            auto startLine = AnchorLine(request.parentOrigin, request.parentExtent, request.anchors.start);
            auto endLine = AnchorLine(request.parentOrigin, request.parentExtent, request.anchors.end);
            if (startLine.HasError() || endLine.HasError())
                return Failure<AnchorLines>(UiErrors::LayoutInvalid);
            return Result<AnchorLines>::Success(
                {startLine.Value(), endLine.Value(), request.anchors.start.has_value(), request.anchors.end.has_value()});
        }

        [[nodiscard]] Result<std::int32_t> ResolveAnchorExtent(const AxisPlacementRequest &request, const std::int32_t segment) {
            if (!IsExplicit(request.preferred))
                return Result<std::int32_t>::Success(segment);
            if (request.anchors.alignment == UiLayoutAlignment::Stretch && !request.anchors.preserveSize)
                return Failure<std::int32_t>(UiErrors::LayoutConstraintConflict);
            return Result<std::int32_t>::Success(request.desired);
        }

        [[nodiscard]] Result<AxisPlacement> AlignAnchorSegment(const AxisPlacementRequest &request, const std::int32_t segmentStart,
                                                               const std::int32_t segmentEnd, const std::int32_t segment,
                                                               const std::int32_t extent) {
            using enum UiLayoutAlignment;
            const auto alignment = request.anchors.alignment == UiLayoutAlignment::Stretch && request.anchors.preserveSize
                                       ? Center
                                       : request.anchors.alignment;
            if (alignment == Stretch || alignment == Start)
                return Result<AxisPlacement>::Success({segmentStart, extent});
            if (alignment == End) {
                const auto origin = CheckedSub(segmentEnd, extent);
                return origin.HasError() ? Failure<AxisPlacement>(UiErrors::LayoutInvalid)
                                         : Result<AxisPlacement>::Success({origin.Value(), extent});
            }
            const auto origin = CheckedCast(static_cast<std::int64_t>(segmentStart) + (static_cast<std::int64_t>(segment) - extent) / 2);
            return origin.HasError() ? Failure<AxisPlacement>(UiErrors::LayoutInvalid)
                                     : Result<AxisPlacement>::Success({origin.Value(), extent});
        }

        [[nodiscard]] Result<AxisPlacement> PlaceBetweenAnchors(const AxisPlacementRequest &request, const AnchorLines &lines) {
            const auto segmentStart = CheckedAdd(lines.start, request.edges.left);
            const auto segmentEnd = CheckedSub(lines.end, request.edges.right);
            if (segmentStart.HasError() || segmentEnd.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            const auto segment =
                CheckedCast(std::max<std::int64_t>(0, static_cast<std::int64_t>(segmentEnd.Value()) - segmentStart.Value()));
            if (segment.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            const auto extent = ResolveAnchorExtent(request, segment.Value());
            if (extent.HasError())
                return Result<AxisPlacement>::Failure(extent.ErrorValue());
            return AlignAnchorSegment(request, segmentStart.Value(), segmentEnd.Value(), segment.Value(), extent.Value());
        }

        [[nodiscard]] Result<AxisPlacement> PlaceFromAnchor(const AxisPlacementRequest &request, const AnchorLines &lines) {
            if (lines.hasStart) {
                const auto offset = CheckedAdd(lines.start, request.edges.left);
                const auto pivotOffset = PivotOffset(request.desired, request.pivot);
                if (offset.HasError() || pivotOffset.HasError())
                    return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
                const auto origin = CheckedSub(offset.Value(), pivotOffset.Value());
                return origin.HasError() ? Failure<AxisPlacement>(UiErrors::LayoutInvalid)
                                         : Result<AxisPlacement>::Success({origin.Value(), request.desired});
            }
            const auto offset = CheckedSub(lines.end, request.edges.right);
            const auto pivotOffset = PivotOffset(request.desired, UiScalarUnitsPerDip - request.pivot);
            if (offset.HasError() || pivotOffset.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            const auto origin = CheckedSub(offset.Value(), pivotOffset.Value());
            return origin.HasError() ? Failure<AxisPlacement>(UiErrors::LayoutInvalid)
                                     : Result<AxisPlacement>::Success({origin.Value(), request.desired});
        }

        [[nodiscard]] Result<AxisPlacement> PlaceInFlow(const AxisPlacementRequest &request) {
            const auto available = CheckedCast(
                std::max<std::int64_t>(0, static_cast<std::int64_t>(request.parentExtent) - request.edges.left - request.edges.right));
            if (available.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            std::int32_t extent = request.desired;
            if (!IsExplicit(request.preferred) && request.flowAlignment == UiLayoutAlignment::Stretch)
                extent = available.Value();
            const auto remaining = CheckedCast(std::max<std::int64_t>(0, static_cast<std::int64_t>(available.Value()) - extent));
            if (remaining.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            std::int32_t alignmentOffset{};
            if (request.flowAlignment == UiLayoutAlignment::Center)
                alignmentOffset = remaining.Value() / 2;
            else if (request.flowAlignment == UiLayoutAlignment::End)
                alignmentOffset = remaining.Value();
            const auto cursorWithEdge = CheckedAdd(request.cursor, request.edges.left);
            if (cursorWithEdge.HasError())
                return Failure<AxisPlacement>(UiErrors::LayoutInvalid);
            const auto origin = CheckedAdd(cursorWithEdge.Value(), alignmentOffset);
            return origin.HasError() ? Failure<AxisPlacement>(UiErrors::LayoutInvalid)
                                     : Result<AxisPlacement>::Success({origin.Value(), extent});
        }
    }  // namespace

    [[nodiscard]] bool IsOutOfFlow(const UiLayoutStyle &style) noexcept {
        return style.positioning == UiLayoutPositioning::Absolute || HasAnchor(style.anchors.horizontal) ||
               HasAnchor(style.anchors.vertical);
    }

    [[nodiscard]] Result<AxisPlacement> PlaceAxis(const AxisPlacementRequest &request) {
        auto lines = ResolveAnchorLines(request);
        if (lines.HasError())
            return Result<AxisPlacement>::Failure(lines.ErrorValue());
        if (lines.Value().hasStart && lines.Value().hasEnd)
            return PlaceBetweenAnchors(request, lines.Value());
        if (lines.Value().hasStart || lines.Value().hasEnd)
            return PlaceFromAnchor(request, lines.Value());
        return PlaceInFlow(request);
    }

    [[nodiscard]] Result<ChildAxisBounds> ResolveChildAxis(const UiLength &preferred, const UiLength &minimum, const UiLength &maximum,
                                                           const std::int32_t available) {
        auto minValue = ResolveLength(minimum, available);
        auto maxValue = ResolveOptionalMaximum(maximum, available);
        if (minValue.HasError() || maxValue.HasError())
            return Failure<ChildAxisBounds>(UiErrors::LayoutInvalid);
        if (IsExplicit(preferred)) {
            auto explicitValue = ResolveLength(preferred, available);
            if (explicitValue.HasError())
                return Failure<ChildAxisBounds>(UiErrors::LayoutInvalid);
            minValue = explicitValue;
            maxValue = explicitValue;
        } else if (minValue.Value() > maxValue.Value()) {
            maxValue = minValue;
        }
        return Result<ChildAxisBounds>::Success({minValue.Value(), maxValue.Value()});
    }

    [[nodiscard]] const UiLayoutElementDescriptor *FindDescriptor(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                  const UiElementHandle element) noexcept {
        const auto found = std::ranges::lower_bound(descriptors, element, {}, &UiLayoutElementDescriptor::element);
        return found != descriptors.end() && found->element == element ? std::to_address(found) : nullptr;
    }

    [[nodiscard]] bool IsHorizontal(const UiLayoutContainerStyle &container) noexcept {
        return container.orientation == UiLayoutOrientation::Horizontal;
    }

    [[nodiscard]] std::int32_t MainValue(const UiLogicalExtent extent, const bool horizontal) noexcept {
        return horizontal ? extent.width : extent.height;
    }

    [[nodiscard]] std::int32_t CrossValue(const UiLogicalExtent extent, const bool horizontal) noexcept {
        return horizontal ? extent.height : extent.width;
    }

    [[nodiscard]] std::int32_t MainMarginStart(const UiLayoutEdges margins, const bool horizontal) noexcept {
        return horizontal ? margins.left : margins.top;
    }

    [[nodiscard]] std::int32_t MainMarginEnd(const UiLayoutEdges margins, const bool horizontal) noexcept {
        return horizontal ? margins.right : margins.bottom;
    }

    [[nodiscard]] std::int32_t CrossMarginStart(const UiLayoutEdges margins, const bool horizontal) noexcept {
        return horizontal ? margins.top : margins.left;
    }

    [[nodiscard]] std::int32_t CrossMarginEnd(const UiLayoutEdges margins, const bool horizontal) noexcept {
        return horizontal ? margins.bottom : margins.right;
    }

    [[nodiscard]] UiLayoutAlignment CrossAlignment(const UiLayoutContainerStyle &container, const UiLayoutStyle &child,
                                                   const bool horizontal) noexcept {
        if (container.crossAlignment != UiLayoutAlignment::Start)
            return container.crossAlignment;
        return horizontal ? child.verticalAlignment : child.horizontalAlignment;
    }

    [[nodiscard]] Result<UiLogicalRect> PlaceAbsoluteChild(const UiLogicalRect parentContent, const UiLayoutElementDescriptor &child,
                                                           const UiLayoutChildMeasurement &measurement) {
        const auto &style = child.style;
        const auto &margins = style.margin;
        const AxisPlacementRequest horizontal{style.anchors.horizontal,
                                              style.width,
                                              {margins.left, 0, margins.right, 0},
                                              style.horizontalAlignment,
                                              parentContent.origin.x,
                                              parentContent.extent.width,
                                              measurement.measurement.desired.width,
                                              style.pivot.x,
                                              parentContent.origin.x};
        const AxisPlacementRequest vertical{style.anchors.vertical,
                                            style.height,
                                            {margins.top, 0, margins.bottom, 0},
                                            style.verticalAlignment,
                                            parentContent.origin.y,
                                            parentContent.extent.height,
                                            measurement.measurement.desired.height,
                                            style.pivot.y,
                                            parentContent.origin.y};
        const auto horizontalPlacement = PlaceAxis(horizontal);
        const auto verticalPlacement = PlaceAxis(vertical);
        if (horizontalPlacement.HasError() || verticalPlacement.HasError())
            return Result<UiLogicalRect>::Failure(horizontalPlacement.HasError() ? horizontalPlacement.ErrorValue()
                                                                                 : verticalPlacement.ErrorValue());
        const auto x = CheckedAdd(horizontalPlacement.Value().origin, style.offsets.left);
        const auto y = CheckedAdd(verticalPlacement.Value().origin, style.offsets.top);
        if (x.HasError() || y.HasError())
            return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
        return Result<UiLogicalRect>::Success(
            {{x.Value(), y.Value()}, {horizontalPlacement.Value().extent, verticalPlacement.Value().extent}});
    }

    [[nodiscard]] Result<UiLogicalRect> PlaceFlowChild(const UiLogicalRect cell, const UiLayoutElementDescriptor &child,
                                                       const UiLayoutChildMeasurement &measurement, const bool horizontal,
                                                       const UiLayoutAlignment alignment, const std::int32_t mainOverride) {
        const auto &style = child.style;
        const auto &margins = style.margin;
        const auto mainAvailable = MainValue(cell.extent, horizontal);
        const auto crossAvailable = CrossValue(cell.extent, horizontal);
        const auto mainStart = MainMarginStart(margins, horizontal);
        const auto mainEnd = MainMarginEnd(margins, horizontal);
        const auto crossStart = CrossMarginStart(margins, horizontal);
        const auto crossEnd = CrossMarginEnd(margins, horizontal);
        const auto measuredMain = mainOverride >= 0 ? mainOverride : MainValue(measurement.measurement.desired, horizontal);
        const auto measuredCross = CrossValue(measurement.measurement.desired, horizontal);
        const auto mainExtent = std::max<std::int32_t>(0, std::min(measuredMain, std::max(0, mainAvailable - mainStart - mainEnd)));
        const bool crossAuto = horizontal ? style.height.kind == UiLengthKind::Auto : style.width.kind == UiLengthKind::Auto;
        const auto crossExtent =
            alignment == UiLayoutAlignment::Stretch && crossAuto
                ? std::max<std::int32_t>(0, crossAvailable - crossStart - crossEnd)
                : std::max<std::int32_t>(0, std::min(measuredCross, std::max(0, crossAvailable - crossStart - crossEnd)));
        const auto crossRemaining = std::max<std::int32_t>(0, crossAvailable - crossStart - crossEnd - crossExtent);
        std::int32_t crossOffset{};
        if (alignment == UiLayoutAlignment::Center)
            crossOffset = crossRemaining / 2;
        else if (alignment == UiLayoutAlignment::End)
            crossOffset = crossRemaining;
        const auto mainOrigin = CheckedAdd(horizontal ? cell.origin.x : cell.origin.y, mainStart);
        const auto crossOrigin = CheckedAdd(horizontal ? cell.origin.y : cell.origin.x, crossStart + crossOffset);
        if (mainOrigin.HasError() || crossOrigin.HasError())
            return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
        const auto x = horizontal ? mainOrigin.Value() : crossOrigin.Value();
        const auto y = horizontal ? crossOrigin.Value() : mainOrigin.Value();
        const auto width = horizontal ? mainExtent : crossExtent;
        const auto height = horizontal ? crossExtent : mainExtent;
        const auto offsetX = CheckedAdd(x, style.offsets.left);
        const auto offsetY = CheckedAdd(y, style.offsets.top);
        if (offsetX.HasError() || offsetY.HasError())
            return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
        return Result<UiLogicalRect>::Success({{offsetX.Value(), offsetY.Value()}, {width, height}});
    }
}  // namespace Horo::Runtime::Ui::LayoutInternal
