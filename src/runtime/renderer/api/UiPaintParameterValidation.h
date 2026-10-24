#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Render::Detail {
    /** @brief Validates the same exact rounded-mask ABI at graph admission and before native upload. */
    [[nodiscard]] inline bool ValidUiPaintMask(const std::array<float, 2> &extent, const std::array<float, 4> &radii) noexcept {
        return std::ranges::all_of(extent,
                                   [](float value) {
            return std::isfinite(value) && value > 0;
        }) &&
               std::ranges::all_of(radii,
                                   [](float value) {
            return std::isfinite(value) && value >= 0;
        }) && radii[0] + radii[1] <= extent[0] &&
               radii[3] + radii[2] <= extent[0] && radii[0] + radii[3] <= extent[1] && radii[1] + radii[2] <= extent[1];
    }
}  // namespace Horo::Render::Detail
