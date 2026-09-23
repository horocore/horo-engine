#pragma once

#include "Horo/Runtime/Ui/UiLayout.h"

#include <cstdint>
#include <span>

namespace Horo::Runtime::Ui::LayoutInternal {
    [[nodiscard]] Result<std::int32_t> CheckedCast(std::int64_t value);
    [[nodiscard]] Result<std::int32_t> CheckedAdd(std::int32_t left, std::int32_t right);
    [[nodiscard]] Result<std::int32_t> CheckedSub(std::int32_t left, std::int32_t right);
    [[nodiscard]] Result<std::int32_t> MultiplyRatio(std::int32_t value, std::uint32_t numerator, std::uint32_t denominator);
    [[nodiscard]] Result<std::int32_t> ResolveLength(const UiLength &length, std::int32_t available, bool autoValueIsZero = true);
    [[nodiscard]] Result<std::int32_t> ResolveOptionalMaximum(const UiLength &length, std::int32_t available);
    [[nodiscard]] Result<std::int32_t> AnchorLine(std::int32_t origin, std::int32_t extent, std::optional<UiScalar> anchor);
    [[nodiscard]] Result<std::int32_t> PivotOffset(std::int32_t extent, std::int32_t pivot);
    [[nodiscard]] Result<UiLogicalRect> MakeRect(UiLogicalPoint origin, UiLogicalExtent extent);
    [[nodiscard]] Result<UiLogicalRect> ExpandRect(UiLogicalRect rect, UiLayoutEdges edges);
    [[nodiscard]] Result<UiLogicalRect> UnionRect(UiLogicalRect first, UiLogicalRect second);

    [[nodiscard]] Result<void> ResolveChildConstraints(std::span<const UiLayoutElementDescriptor> descriptors,
                                                       const UiLayoutChildConstraintRequest &request,
                                                       std::span<UiLayoutConstraints> output);
    [[nodiscard]] Result<UiLayoutArrangement> Arrange(std::span<const UiLayoutElementDescriptor> descriptors,
                                                      const UiLayoutElementDescriptor &parent, const UiLayoutArrangeRequest &request,
                                                      std::span<UiLogicalRect> childContent,
                                                      std::span<UiLayoutChildPlacement> placementScratch,
                                                      std::span<UiLayoutLine> lineScratch);
}  // namespace Horo::Runtime::Ui::LayoutInternal
