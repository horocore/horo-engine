#pragma once

/** @file VfxGpuQualification.h @brief Versioned CPU/native GPU observation comparison policy. */
#include <array>
#include <cmath>
#include <cstdint>

namespace Horo::Vfx::Tests {
    inline constexpr std::uint32_t GpuQualificationPolicyVersion = 1;

    /** @brief Absolute and relative error bounds for one normalized binary32 property. */
    struct GpuPropertyTolerance {
        const char *name;
        float absolute;
        float relative;
    };

    // Observation word order after the two exact particle identity words.
    inline constexpr std::array GpuPropertyTolerances{
        GpuPropertyTolerance{"positionX", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"positionY", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"positionZ", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"velocityX", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"velocityY", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"velocityZ", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"sizeX", 0.0F, 0.0F},
        GpuPropertyTolerance{"sizeY", 0.0F, 0.0F},
        GpuPropertyTolerance{"rotation", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"angularVelocity", 0.0F, 0.0F},
        GpuPropertyTolerance{"age", 0.00002F, 0.0002F},
        GpuPropertyTolerance{"maximumAge", 0.0F, 0.0F},
    };

    /** @brief Rejects nonfinite values and errors exceeding the property's absolute plus reference-relative bound. */
    [[nodiscard]] inline bool GpuPropertyMatches(float actual, float expected, GpuPropertyTolerance tolerance) noexcept {
        return std::isfinite(actual) && std::isfinite(expected) &&
               std::abs(actual - expected) <= tolerance.absolute + tolerance.relative * std::abs(expected);
    }
}  // namespace Horo::Vfx::Tests
