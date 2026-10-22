#pragma once

/** @file TerrainBrushAdmission.h @brief Private arithmetic admission shared by brush planning and compile-time tests. */
#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Horo::Editor::Detail {
    /** @brief Narrows admitted finite heights; denial leaves the output unchanged. */
    [[nodiscard]] constexpr bool TryNarrowTerrainHeight(double height, float &output) noexcept {
        output = static_cast<float>(height);
        return output >= -std::numeric_limits<float>::max() && output <= std::numeric_limits<float>::max();
    }

    /** @brief Admits one patch's dynamic object storage and height payload without overflowing. */
    [[nodiscard]] constexpr bool CanOwnTerrainHeightPatch(std::uint64_t samples, std::size_t maximumOwnedBytes) noexcept {
        return samples <= maximumOwnedBytes / sizeof(float);
    }
}  // namespace Horo::Editor::Detail
