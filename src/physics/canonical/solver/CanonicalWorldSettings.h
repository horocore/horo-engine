#pragma once

/** @file CanonicalWorldSettings.h
 * @brief Native-only translation of an already validated immutable settings snapshot.
 */

#include "Horo/Physics/PhysicsWorldSettings.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/PhysicsSettings.h>

namespace Horo::Physics::Detail {
    /**
     * @brief Owned native preparation values plus the exact Horo policy that the world owner must enforce.
     * Source retains bounds, shape/joint/collider/plan limits, command/event/query work budgets,
     * scratch policy and containment; none is silently translated into unrelated native knobs.
     * The world owner must implement each required admission/containment path or reject creation.
     * This value allocates no native world, allocator, factory, jobs or published identity.
     */
    struct CanonicalWorldSettings final {
        PhysicsWorldSettings source;
        JPH::PhysicsSettings solver;
        JPH::Vec3 gravity;
        JPH::EMotionQuality defaultMotionQuality;
        float fixedDeltaSeconds;
        int collisionSteps;
        std::uint32_t maximumBodies;
        std::uint32_t maximumBodyPairs;
        std::uint32_t maximumContactConstraints;
        std::uint64_t scratchBytes;
    };

    /** @brief Maps a known Horo motion-quality value without admitting it into a world profile.
     * @param quality Typed requested representation; complete snapshot validation owns profile admission.
     * @return Native representation or OperationUnsupported for an unknown value.
     */
    [[nodiscard]] Result<JPH::EMotionQuality> TranslateDefaultMotionQuality(PhysicsDefaultMotionQuality quality);

    /**
     * @brief Translates supported settings explicitly after linked-binary compatibility validation.
     * @param settings Validated immutable snapshot; construction cannot bypass capture validation.
     * @return Owned preparation values or ProfileUnsupported if native ABI verification fails.
     * @post Performs no registration, native allocation, world activation or capability advertisement.
     * The native deterministic-order flag is not a Horo deterministic qualification claim.
     */
    [[nodiscard]] Result<CanonicalWorldSettings> TranslateCanonicalWorldSettings(const PhysicsWorldSettings &settings);
}  // namespace Horo::Physics::Detail
