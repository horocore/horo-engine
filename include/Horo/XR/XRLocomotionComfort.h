#pragma once

/** @file XRLocomotionComfort.h @brief Typed XR locomotion comfort preferences shared by project and gameplay owners. */

namespace Horo::XR {
    /** @brief Copied, renderer-independent comfort settings for one gameplay locomotion binding. */
    struct XRLocomotionComfortSettings final {
        bool reducedMotion{}; /**< Suppresses continuous artificial travel and retains supported physical and discrete movement. */
        float maximumVignetteOpacity{};            /**< Finite [0, 1]. Zero disables the artificial-motion vignette. */
        bool teleportWhenReduced{};                /**< Enables the configured destination owner as a reduced-motion alternative. */
        float artificialSpeedCapMetersPerSecond{}; /**< Finite nonnegative cap for continuous travel. Zero keeps the authored cap. */
        float snapTurnAngleRadians{}; /**< Finite [0, pi] override for an authored snap route. Zero keeps its authored angle. */
    };
}  // namespace Horo::XR
