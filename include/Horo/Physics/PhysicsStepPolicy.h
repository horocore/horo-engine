#pragma once

/** @file PhysicsStepPolicy.h
 * @brief Canonical fixed-step solver policy captured by an immutable world settings snapshot.
 */

#include "Horo/Physics/PhysicsContinuousCollision.h"

#include <cstdint>

namespace Horo::Physics {
    /**
     * @brief Numeric step policy in SI units; these values do not advance or activate a solver.
     *
     * CanonicalV1 currently admits one substep per fixed tick, velocity/position iterations 10/2,
     * sleeping enabled at 0.03 m/s for 0.5 s and bounded discrete/linear-cast defaults. Other schedules,
     * sleep overrides require explicit profile/capability admission, not silent
     * native defaults. Individual CCD body policy belongs to body admission rather than this default.
     */
    struct PhysicsStepPolicy final {
        std::uint32_t substepsPerTick{1};
        std::uint32_t velocityIterations{10};
        std::uint32_t positionIterations{2};
        float sleepPointSpeedMetersPerSecond{0.03F};
        float sleepDelaySeconds{0.5F};
        bool sleepingEnabled{true};
        PhysicsDefaultMotionQuality defaultMotionQuality{PhysicsDefaultMotionQuality::Discrete};

        float linearCastThresholdFraction{0.75F};
        float linearCastPenetrationFraction{0.25F};

        bool operator==(const PhysicsStepPolicy &) const noexcept = default;
    };

    /**
     * @brief Validates representation and currently admitted CanonicalV1 policy without clamping or mutation.
     * @param policy Requested solver work/sleep/default-motion policy.
     * @return Success; DescriptorInvalid for non-finite/non-positive numeric values; OperationUnsupported
     * for an unknown motion-quality value; ProfileUnsupported for a well-formed but unadmitted policy.
     * @pre Preparation/control use; diagnostics may allocate.
     * @post No native settings, world, timer or work budget is created. Fixed delta and resource/tick
     * budgets are separately validated by the complete world settings snapshot.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStepPolicy(const PhysicsStepPolicy &policy);
}  // namespace Horo::Physics
