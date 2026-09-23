#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiLayoutArrangementInternal.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <cstddef>

namespace Horo::Runtime::Ui::LayoutInternal {
    namespace {
        struct LegacyChildArrangement final {
            UiLogicalRect content;
            bool advancesFlow{};
            std::int32_t nextCursor{};
        };

        [[nodiscard]] Result<std::int32_t> AdvanceLegacyFlowCursor(const UiLayoutStyle &style, const std::int32_t origin,
                                                                   const std::int32_t extent) {
            auto next = CheckedAdd(origin, extent);
            if (next.HasError())
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            return CheckedAdd(next.Value(), style.margin.bottom);
        }

        [[nodiscard]] Result<LegacyChildArrangement> ArrangeLegacyChild(const UiLogicalRect parentContent,
                                                                        const UiLayoutElementDescriptor &child,
                                                                        const UiLayoutChildMeasurement &measurement,
                                                                        const std::int32_t cursorY) {
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
                return Result<LegacyChildArrangement>::Failure(horizontalPlacement.HasError() ? horizontalPlacement.ErrorValue()
                                                                                              : verticalPlacement.ErrorValue());
            const auto x = CheckedAdd(horizontalPlacement.Value().origin, style.offsets.left);
            const auto y = CheckedAdd(verticalPlacement.Value().origin, style.offsets.top);
            if (x.HasError() || y.HasError())
                return Failure<LegacyChildArrangement>(UiErrors::LayoutInvalid);
            const UiLogicalRect content{{x.Value(), y.Value()}, {horizontalPlacement.Value().extent, verticalPlacement.Value().extent}};
            LegacyChildArrangement result{content, false, cursorY};
            if (!outOfFlow) {
                auto next = AdvanceLegacyFlowCursor(style, y.Value(), verticalPlacement.Value().extent);
                if (next.HasError())
                    return Result<LegacyChildArrangement>::Failure(next.ErrorValue());
                result.advancesFlow = true;
                result.nextCursor = next.Value();
            }
            return Result<LegacyChildArrangement>::Success(result);
        }

        [[nodiscard]] Result<UiLogicalRect> ArrangeLegacyChildren(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                  const UiLogicalRect parentContent,
                                                                  const std::span<const UiLayoutChildMeasurement> children,
                                                                  const std::span<UiLogicalRect> output) {
            UiLogicalRect overflow = parentContent;
            std::int32_t cursorY = parentContent.origin.y;
            for (std::size_t index = 0; index < children.size(); ++index) {
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure<UiLogicalRect>(UiErrors::LayoutSourceStale);
                auto arranged = ArrangeLegacyChild(parentContent, *child, children[index], cursorY);
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

        [[nodiscard]] Result<UiLogicalRect> ArrangeChildren(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                            const UiLogicalRect parentContent,
                                                            const std::span<const UiLayoutChildMeasurement> children,
                                                            const UiLayoutStyle &parentStyle, const std::span<UiLogicalRect> output,
                                                            const std::span<UiLayoutChildPlacement> placementScratch,
                                                            const std::span<UiLayoutLine> lineScratch) {
            if (parentStyle.container.kind == UiLayoutContainerKind::Stack &&
                parentStyle.container.orientation == UiLayoutOrientation::Vertical &&
                parentStyle.container.wrap == UiLayoutWrapMode::NoWrap && parentStyle.container.gap == 0 &&
                parentStyle.container.mainAlignment == UiLayoutDistribution::Start &&
                parentStyle.container.crossAlignment == UiLayoutAlignment::Start && parentStyle.container.columnCount == 0 &&
                parentStyle.container.rowCount == 0)
                return ArrangeLegacyChildren(descriptors, parentContent, children, output);
            if (const auto arranged =
                    parentStyle.container.kind == UiLayoutContainerKind::Grid
                        ? ArrangeGrid(descriptors, parentContent, children, parentStyle, output, placementScratch)
                        : ArrangeStackOrFlex(descriptors, parentContent, children, parentStyle, output, placementScratch, lineScratch);
                arranged.HasError())
                return Result<UiLogicalRect>::Failure(arranged.ErrorValue());
            UiLogicalRect overflow = parentContent;
            for (std::size_t index = 0; index < children.size(); ++index) {
                auto unioned = UnionRect(overflow, output[index]);
                if (unioned.HasError())
                    return Failure<UiLogicalRect>(UiErrors::LayoutInvalid);
                overflow = unioned.Value();
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
                                        const std::span<UiLogicalRect> childContent,
                                        const std::span<UiLayoutChildPlacement> placementScratch,
                                        const std::span<UiLayoutLine> lineScratch) {
        auto boxes = ResolveArrangeBoxes(request.assignedContent, parent.style);
        if (boxes.HasError())
            return Result<UiLayoutArrangement>::Failure(boxes.ErrorValue());
        auto overflow = ArrangeChildren(descriptors, request.assignedContent, request.children, parent.style, childContent,
                                        placementScratch, lineScratch);
        if (overflow.HasError())
            return Result<UiLayoutArrangement>::Failure(overflow.ErrorValue());
        return Result<UiLayoutArrangement>::Success({boxes.Value().margin, boxes.Value().border, boxes.Value().padding,
                                                     request.assignedContent, overflow.Value(), boxes.Value().border,
                                                     request.measurement.baseline});
    }
}  // namespace Horo::Runtime::Ui::LayoutInternal
