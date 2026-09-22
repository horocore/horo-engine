#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace Horo::Runtime::Ui::LayoutInternal {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsExplicit(const UiLength &length) noexcept {
            return length.kind != UiLengthKind::Auto;
        }

        [[nodiscard]] bool HasAnchor(const UiLayoutAnchorAxis &axis) noexcept {
            return axis.start.has_value() || axis.end.has_value();
        }

        [[nodiscard]] bool IsOutOfFlow(const UiLayoutStyle &style) noexcept {
            return style.positioning == UiLayoutPositioning::Absolute || HasAnchor(style.anchors.horizontal) ||
                   HasAnchor(style.anchors.vertical);
        }

        struct AxisPlacement final {
            std::int32_t origin{};
            std::int32_t extent{};
        };

        struct AxisPlacementRequest final {
            const UiLayoutAnchorAxis &anchors;
            const UiLength &preferred;
            UiLayoutEdges edges;
            UiLayoutAlignment flowAlignment;
            std::int32_t parentOrigin;
            std::int32_t parentExtent;
            std::int32_t desired;
            std::int32_t pivot;
            std::int32_t cursor;
        };

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

        struct ChildAxisBounds final {
            std::int32_t minimum{};
            std::int32_t maximum{};
        };

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

        struct ChildArrangement final {
            UiLogicalRect content;
            bool advancesFlow{};
            std::int32_t nextCursor{};
        };

        [[nodiscard]] Result<std::int32_t> AdvanceFlowCursor(const UiLayoutStyle &style, const std::int32_t origin,
                                                             const std::int32_t extent) {
            auto next = CheckedAdd(origin, extent);
            if (next.HasError())
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            next = CheckedAdd(next.Value(), style.margin.bottom);
            return next;
        }

        [[nodiscard]] Result<ChildArrangement> ArrangeChild(const UiLogicalRect parentContent, const UiLayoutElementDescriptor &child,
                                                            const UiLayoutChildMeasurement &measurement, const std::int32_t cursorY) {
            const auto &style = child.style;
            const auto &margins = style.margin;
            const bool outOfFlow = IsOutOfFlow(style);
            const auto placementCursorY = outOfFlow ? parentContent.origin.y : cursorY;
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
                                                placementCursorY};
            const auto horizontalPlacement = PlaceAxis(horizontal);
            const auto verticalPlacement = PlaceAxis(vertical);
            if (horizontalPlacement.HasError() || verticalPlacement.HasError())
                return Result<ChildArrangement>::Failure(horizontalPlacement.HasError() ? horizontalPlacement.ErrorValue()
                                                                                        : verticalPlacement.ErrorValue());
            const auto x = CheckedAdd(horizontalPlacement.Value().origin, style.offsets.left);
            const auto y = CheckedAdd(verticalPlacement.Value().origin, style.offsets.top);
            if (x.HasError() || y.HasError())
                return Failure<ChildArrangement>(UiErrors::LayoutInvalid);
            const UiLogicalRect content{{x.Value(), y.Value()}, {horizontalPlacement.Value().extent, verticalPlacement.Value().extent}};
            ChildArrangement result{content, false, cursorY};
            if (!outOfFlow) {
                auto next = AdvanceFlowCursor(style, y.Value(), verticalPlacement.Value().extent);
                if (next.HasError())
                    return Result<ChildArrangement>::Failure(next.ErrorValue());
                result.advancesFlow = true;
                result.nextCursor = next.Value();
            }
            return Result<ChildArrangement>::Success(result);
        }

        struct ArrangeBoxes final {
            UiLogicalRect margin;
            UiLogicalRect border;
            UiLogicalRect padding;
        };

        [[nodiscard]] Result<ArrangeBoxes> ResolveArrangeBoxes(const UiLogicalRect assignedContent, const UiLayoutStyle &style) {
            const auto padding = ExpandRect(assignedContent, style.padding);
            if (padding.HasError())
                return Failure<ArrangeBoxes>(UiErrors::LayoutInvalid);
            const auto border = ExpandRect(padding.Value(), style.border);
            if (border.HasError())
                return Failure<ArrangeBoxes>(UiErrors::LayoutInvalid);
            const auto margin = ExpandRect(border.Value(), style.margin);
            if (margin.HasError())
                return Failure<ArrangeBoxes>(UiErrors::LayoutInvalid);
            return Result<ArrangeBoxes>::Success({margin.Value(), border.Value(), padding.Value()});
        }

        [[nodiscard]] const UiLayoutElementDescriptor *FindDescriptor(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                      const UiElementHandle element) noexcept {
            const auto found = std::ranges::lower_bound(descriptors, element, {}, &UiLayoutElementDescriptor::element);
            return found != descriptors.end() && found->element == element ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] Result<UiLogicalRect> ArrangeChildren(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                            const UiLogicalRect parentContent,
                                                            const std::span<const UiLayoutChildMeasurement> children,
                                                            const std::span<UiLogicalRect> output) {
            UiLogicalRect overflow = parentContent;
            std::int32_t cursorY = parentContent.origin.y;
            for (std::size_t index = 0; index < children.size(); ++index) {
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure<UiLogicalRect>(UiErrors::LayoutSourceStale);
                auto arranged = ArrangeChild(parentContent, *child, children[index], cursorY);
                if (arranged.HasError())
                    return Result<UiLogicalRect>::Failure(arranged.ErrorValue());
                output[index] = arranged.Value().content;
                auto unioned = UnionRect(overflow, output[index]);
                if (unioned.HasError())
                    return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
                overflow = unioned.Value();
                if (arranged.Value().advancesFlow)
                    cursorY = arranged.Value().nextCursor;
            }
            return Result<UiLogicalRect>::Success(overflow);
        }
    }  // namespace

    /** @brief Resolve direct child constraints from layout descriptors. */
    Result<void> ResolveChildConstraints(const std::span<const UiLayoutElementDescriptor> descriptors,
                                         const UiLayoutChildConstraintRequest &request, const std::span<UiLayoutConstraints> output) {
        for (std::size_t index = 0; index < request.children.size(); ++index) {
            const auto *descriptor = FindDescriptor(descriptors, request.children[index]);
            if (descriptor == nullptr)
                return Failure(UiErrors::LayoutSourceStale);
            const auto width = ResolveChildAxis(descriptor->style.width, descriptor->style.minimumWidth, descriptor->style.maximumWidth,
                                                request.constraints.maximum.width);
            const auto height = ResolveChildAxis(descriptor->style.height, descriptor->style.minimumHeight, descriptor->style.maximumHeight,
                                                 request.constraints.maximum.height);
            if (width.HasError() || height.HasError())
                return Failure(UiErrors::LayoutInvalid);
            output[index] = {{width.Value().minimum, height.Value().minimum}, {width.Value().maximum, height.Value().maximum}};
        }
        return Result<void>::Success();
    }

    /** @brief Arrange direct children and publish their content rectangles. */
    Result<UiLayoutArrangement> Arrange(const std::span<const UiLayoutElementDescriptor> descriptors,
                                        const UiLayoutElementDescriptor &parent, const UiLayoutArrangeRequest &request,
                                        const std::span<UiLogicalRect> childContent) {
        auto boxes = ResolveArrangeBoxes(request.assignedContent, parent.style);
        if (boxes.HasError())
            return Result<UiLayoutArrangement>::Failure(boxes.ErrorValue());
        auto overflow = ArrangeChildren(descriptors, request.assignedContent, request.children, childContent);
        if (overflow.HasError())
            return Result<UiLayoutArrangement>::Failure(overflow.ErrorValue());
        return Result<UiLayoutArrangement>::Success({boxes.Value().margin, boxes.Value().border, boxes.Value().padding,
                                                     request.assignedContent, overflow.Value(), boxes.Value().border,
                                                     request.measurement.baseline});
    }
}  // namespace Horo::Runtime::Ui::LayoutInternal
