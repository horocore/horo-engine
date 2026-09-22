#pragma once

#include "Horo/Runtime/Ui/UiLayout.h"

#include <cstdint>
#include <limits>
#include <span>

namespace Horo::Runtime::Ui::LayoutInternal {
    template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    inline constexpr std::uint32_t NoPlacementLine = std::numeric_limits<std::uint32_t>::max();

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

    struct ChildAxisBounds final {
        std::int32_t minimum{};
        std::int32_t maximum{};
    };

    struct FlowLineBuildContext final {
        std::span<const UiLayoutElementDescriptor *> childDescriptors;
        std::span<const UiLayoutChildMeasurement> children;
        const UiLayoutContainerStyle &container;
        bool horizontal;
        std::int32_t parentMain;
        std::int32_t parentCross;
        std::span<UiLayoutChildPlacement> placementScratch;
        std::span<UiLayoutLine> lineScratch;
    };

    [[nodiscard]] bool IsOutOfFlow(const UiLayoutStyle &style) noexcept;
    [[nodiscard]] Result<AxisPlacement> PlaceAxis(const AxisPlacementRequest &request);
    [[nodiscard]] Result<ChildAxisBounds> ResolveChildAxis(const UiLength &preferred, const UiLength &minimum, const UiLength &maximum,
                                                           std::int32_t available);
    [[nodiscard]] const UiLayoutElementDescriptor *FindDescriptor(std::span<const UiLayoutElementDescriptor> descriptors,
                                                                  UiElementHandle element) noexcept;

    [[nodiscard]] bool IsHorizontal(const UiLayoutContainerStyle &container) noexcept;
    [[nodiscard]] std::int32_t MainValue(UiLogicalExtent extent, bool horizontal) noexcept;
    [[nodiscard]] std::int32_t CrossValue(UiLogicalExtent extent, bool horizontal) noexcept;
    [[nodiscard]] std::int32_t MainMarginStart(UiLayoutEdges margins, bool horizontal) noexcept;
    [[nodiscard]] std::int32_t MainMarginEnd(UiLayoutEdges margins, bool horizontal) noexcept;
    [[nodiscard]] std::int32_t CrossMarginStart(UiLayoutEdges margins, bool horizontal) noexcept;
    [[nodiscard]] std::int32_t CrossMarginEnd(UiLayoutEdges margins, bool horizontal) noexcept;
    [[nodiscard]] UiLayoutAlignment CrossAlignment(const UiLayoutContainerStyle &container, const UiLayoutStyle &child,
                                                   bool horizontal) noexcept;
    [[nodiscard]] Result<std::int32_t> RoundDivideEven(std::int64_t numerator, std::uint32_t denominator);
    [[nodiscard]] Result<std::int32_t> DistributedOffset(std::int32_t total, std::uint32_t numerator, std::uint32_t denominator);
    [[nodiscard]] Result<std::int32_t> DistributedDelta(std::int32_t total, std::uint32_t start, std::uint32_t end,
                                                        std::uint32_t denominator);
    [[nodiscard]] std::uint64_t SafeWeightedShare(std::uint64_t value, std::uint64_t numerator, std::uint64_t denominator) noexcept;

    [[nodiscard]] Result<UiLogicalRect> PlaceAbsoluteChild(UiLogicalRect parentContent, const UiLayoutElementDescriptor &child,
                                                           const UiLayoutChildMeasurement &measurement);
    [[nodiscard]] Result<UiLogicalRect> PlaceFlowChild(UiLogicalRect cell, const UiLayoutElementDescriptor &child,
                                                       const UiLayoutChildMeasurement &measurement, bool horizontal,
                                                       UiLayoutAlignment alignment, std::int32_t mainOverride = -1);

    [[nodiscard]] Result<void> ArrangeStackOrFlex(std::span<const UiLayoutElementDescriptor> descriptors, UiLogicalRect parentContent,
                                                  std::span<const UiLayoutChildMeasurement> children, const UiLayoutStyle &parentStyle,
                                                  std::span<UiLogicalRect> output, std::span<UiLayoutChildPlacement> placementScratch,
                                                  std::span<UiLayoutLine> lineScratch);
    [[nodiscard]] Result<void> ArrangeGrid(std::span<const UiLayoutElementDescriptor> descriptors, UiLogicalRect parentContent,
                                           std::span<const UiLayoutChildMeasurement> children, const UiLayoutStyle &parentStyle,
                                           std::span<UiLogicalRect> output, std::span<UiLayoutChildPlacement> placementScratch);
}  // namespace Horo::Runtime::Ui::LayoutInternal
