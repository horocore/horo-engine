#pragma once

/** @file PhysicsConstraintDescriptor.h
 * @brief Inert runtime constraint requests with explicit body-local and world-anchor frames.
 */

#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsPose.h"

#include <limits>
#include <numbers>
#include <variant>

namespace Horo::Physics {
    struct PhysicsCapabilities;

    /** @brief Non-owning body endpoint and anchor frame relative to that body's pose, not its center of mass. */
    struct PhysicsBodyAnchor final {
        BodyHandle body;
        PhysicsPose localFrame;
    };

    /**
     * @brief Explicit fixed-world endpoint in the receiving world's current origin frame.
     * The enclosing owner-thread operation must bind/revalidate the origin epoch before use;
     * this numeric pose is not a durable anchor across origin shifts or a queued-command envelope.
     */
    struct PhysicsWorldAnchor final {
        PhysicsPose frame;
    };

    /** @brief Preserve the relative transform defined by the two supplied anchor frames. */
    struct PhysicsFixedConstraint final {};

    /** @brief Allowed separation in meters between the two anchor origins; no spring or motor is implied. */
    struct PhysicsDistanceConstraint final {
        float minimumMeters{};
        float maximumMeters{1.0F};
    };

    /** @brief Rotation about each anchor frame's local +Y axis; local +X defines zero angle. */
    struct PhysicsHingeConstraint final {
        float minimumRadians{-std::numbers::pi_v<float>};
        float maximumRadians{std::numbers::pi_v<float>};
    };

    /** @brief Translation along each anchor frame's local +X axis; local +Y fixes orientation. */
    struct PhysicsSliderConstraint final {
        float minimumMeters{-std::numeric_limits<float>::max()};
        float maximumMeters{std::numeric_limits<float>::max()};
    };

    /** @brief Kind of single-axis coordinate returned by a resident joint. */
    enum class PhysicsJointCoordinateKind : std::uint8_t {
        AngleRadians,
        PositionMeters
    };

    /** @brief Non-owning owner-thread snapshot of a hinge angle or slider displacement. */
    struct PhysicsJointState final {
        PhysicsJointCoordinateKind kind;
        float coordinate{};
    };

    /** @brief Whether two body endpoints may generate contacts while their joint exists. */
    enum class PhysicsJointCollisionPolicy : std::uint8_t {
        DisableBetweenBodies,
        AllowBetweenBodies
    };

    /**
     * @brief Owned structural runtime request; contains no native state, resource lease or published constraint identity.
     *
     * The first endpoint is always a body; the second is a body or an explicit world anchor.
     * Defaults deliberately leave the first handle invalid. Fixed, distance, hinge and slider
     * are canonical runtime operations. Drive, break, spring and other joint kinds need separate policies.
     *
     * These published-handle requests are not serializable scene authoring or detached scene-plan
     * references. Candidate construction uses private resolved plan indexes until aggregate publication.
     */
    struct PhysicsConstraintDescriptor final {
        PhysicsBodyAnchor first;
        std::variant<PhysicsBodyAnchor, PhysicsWorldAnchor> second{PhysicsWorldAnchor{}};
        std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint, PhysicsHingeConstraint, PhysicsSliderConstraint> parameters;
        PhysicsJointCollisionPolicy collisionPolicy{PhysicsJointCollisionPolicy::DisableBetweenBodies};
    };

    /**
     * @brief Validates endpoint ownership, distinct bodies, finite unit frames and joint limits.
     * @param descriptor Immutable runtime request; validation neither repairs frames nor creates missing bodies.
     * @param expectedWorld Published world generation receiving the request.
     * @return Success or a stable Physics handle/world/descriptor error with actionable context.
     * @pre Control/owner-thread use; diagnostics may allocate.
     * @post Success does not prove handle liveness, compatible body modes, admitted geometry or native
     * constraint support. The world must resolve current generations, bind the origin epoch, admit
     * capabilities/budgets and retain both body leases at the structural safe point before publication.
     */
    [[nodiscard]] Result<void> ValidatePhysicsConstraintDescriptor(const PhysicsConstraintDescriptor &descriptor,
                                                                   PhysicsWorldId expectedWorld);

    /**
     * @brief Validates one structural request and requires explicit constraint capability evidence.
     * @param descriptor Immutable backend-neutral constraint request.
     * @param expectedWorld Published world generation receiving the request.
     * @param capabilities Exact immutable capability snapshot retained for this admission attempt.
     * @param expectedRevision Non-zero capability revision retained by the caller.
     * @return Descriptor/identity failure first, otherwise stale, unsupported or unavailable capability evidence.
     * @pre Control/owner-thread use; diagnostics may allocate.
     * @post No handle is published, body lease retained, native constraint created or world state mutated.
     */
    [[nodiscard]] Result<void> AdmitPhysicsConstraintDescriptor(const PhysicsConstraintDescriptor &descriptor, PhysicsWorldId expectedWorld,
                                                                const PhysicsCapabilities &capabilities, std::uint64_t expectedRevision);
}  // namespace Horo::Physics
