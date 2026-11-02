#pragma once
/** @file VfxWorldCoordinates.h @brief Shared private CPU/GPU extraction coordinate conversion. */
#include "Horo/Math/WorldCoordinate64.h"

#include <cmath>
#include <limits>

namespace Horo::Vfx::Detail {
    /** @brief Computes an exact integer cell magnitude before widening; nearby extreme cells retain their relative precision. */
    [[nodiscard]] inline double VfxCellDifference(std::int64_t simulation, std::int64_t render) noexcept {
        const auto a = static_cast<std::uint64_t>(simulation);
        const auto b = static_cast<std::uint64_t>(render);
        return simulation >= render ? static_cast<double>(a - b) : -static_cast<double>(b - a);
    }

    /** @brief Converts relative cells/offset/local value; unrepresentable nonfinite float output fails extraction validation. */
    [[nodiscard]] inline float VfxRelativeAxis(Math::WorldCoordinateAxis64 simulation, Math::WorldCoordinateAxis64 render,
                                               float local = 0) noexcept {
        const auto cells = VfxCellDifference(simulation.cellIndex, render.cellIndex);
        const auto offset = static_cast<double>(simulation.offsetMillimeters) - render.offsetMillimeters;
        const auto value = cells * 1024.0 + offset / 1000.0 + static_cast<double>(local);
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            return std::numeric_limits<float>::quiet_NaN();
        return static_cast<float>(value);
    }
}  // namespace Horo::Vfx::Detail
