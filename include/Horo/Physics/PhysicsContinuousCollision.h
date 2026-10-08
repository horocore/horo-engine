#pragma once

/** @file PhysicsContinuousCollision.h
 * @brief Solver-neutral linear continuous-collision policy and effective runtime evidence.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsIdentity.h"

#include <cstdint>
#include <optional>

namespace Horo::Physics {
    /** @brief Requested linear motion quality; no angular sweep or native solver authority is implied. */
    enum class PhysicsDefaultMotionQuality : std::uint8_t {
        Discrete,
        LinearCast
    };

    /** @brief Optional body override; absence inherits the world's policy only for eligible dynamic bodies. */
    struct PhysicsBodyContinuousCollision final {
        std::optional<PhysicsDefaultMotionQuality> mode;
        bool operator==(const PhysicsBodyContinuousCollision &) const noexcept = default;
    };

    /**
     * @brief Owned world CCD policy, applied only at the joined owner-thread pre-step safe point.
     * @details Fractions use the admitted shape's positive inner radius. Linear casting starts when
     * actual integrated displacement exceeds thresholdFraction times that radius. Penetration is
     * capped by penetrationFraction times the radius and the solver's penetration slop. This does
     * not sweep angular rotation, sensors or kinematic bodies. Per-body threshold overrides are
     * unavailable; the threshold belongs to this single world policy.
     */
    struct PhysicsContinuousCollisionPolicy final {
        PhysicsDefaultMotionQuality defaultMode{PhysicsDefaultMotionQuality::Discrete};
        float thresholdFraction{0.75F};
        float penetrationFraction{0.25F};
        bool operator==(const PhysicsContinuousCollisionPolicy &) const noexcept = default;
    };

    /** @brief Copied effective policy evidence; revision is scoped to the current world, never a settings identity. */
    struct PhysicsContinuousCollisionObservation final {
        PhysicsContinuousCollisionPolicy policy;
        std::uint64_t revision{1};
        PhysicsWorldId world; /**< Exact active-world generation bound by the public owner read. */
    };

    /**
     * @brief Validate bounded world policy without allocating native state or changing a world.
     * @param policy Owned mode and finite dimensionless fractions.
     * @return DescriptorInvalid for fractions outside (0,1], OperationUnsupported for unknown mode, or success.
     * @post Input is unchanged; success grants no world or solver capability.
     */
    [[nodiscard]] Result<void> ValidatePhysicsContinuousCollisionPolicy(const PhysicsContinuousCollisionPolicy &policy);
}  // namespace Horo::Physics
