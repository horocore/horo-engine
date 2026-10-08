#pragma once

/** @file PhysicsBodyDescriptor.h
 * @brief Owned runtime body policy independent of native solver state and authored component identity.
 */

#include "Horo/Physics/PhysicsContinuousCollision.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsPose.h"

#include <cstdint>
#include <variant>

namespace Horo::Physics {
    /** @brief Explicit body transform authority; unknown values are unsupported, never mapped to a default. */
    enum class PhysicsMotionType : std::uint8_t {
        Static,
        Kinematic,
        Dynamic
    };

    /** @brief Static/kinematic policy with no solver-integrated mass; not a zero-mass dynamic body. */
    struct PhysicsNoMass final {};

    /** @brief Requested dynamic mass in kilograms; inertia must be derived from admitted geometry separately. */
    struct PhysicsMass final {
        float kilograms{1.0F};
    };

    /** @brief Requested dynamic density in kg/m³; requires admitted closed geometry for mass/inertia derivation. */
    struct PhysicsDensity final {
        float kilogramsPerCubicMeter{1'000.0F};
    };

    /** @brief Explicit authored/runtime mass intent; absence is typed rather than encoded as zero. */
    using PhysicsMassPolicy = std::variant<PhysicsNoMass, PhysicsMass, PhysicsDensity>;

    /** @brief CanonicalV1 minimum explicit mass in kilograms. */
    inline constexpr float MinimumPhysicsMassKilograms = 0.001F;
    /** @brief CanonicalV1 minimum density in kg/m³. */
    inline constexpr float MinimumPhysicsDensity = 0.001F;
    /** @brief CanonicalV1 maximum explicit mass in kilograms. */
    inline constexpr float MaximumPhysicsMassKilograms = 1.0e9F;
    /** @brief CanonicalV1 maximum density in kg/m³. */
    inline constexpr float MaximumPhysicsDensity = 1.0e7F;
    /** @brief Normative Physics architecture's maximum ordinary linear speed in m/s. */
    inline constexpr double MaximumPhysicsLinearSpeed = 500.0;
    /** @brief CanonicalV1 maximum angular speed in radians/s. */
    inline constexpr float MaximumPhysicsAngularSpeed = 100.0F;
    /** @brief CanonicalV1 maximum depenetration correction speed in m/s. */
    inline constexpr float MaximumPhysicsDepenetrationSpeed = 100.0F;
    /** @brief CanonicalV1 maximum exponential damping coefficient in inverse seconds. */
    inline constexpr float MaximumPhysicsDampingPerSecond = 100.0F;

    /** @brief Independently lockable translation and rotation degrees of freedom. */
    enum class PhysicsAxisLock : std::uint8_t {
        None = 0,
        TranslationX = 1U << 0U,
        TranslationY = 1U << 1U,
        TranslationZ = 1U << 2U,
        RotationX = 1U << 3U,
        RotationY = 1U << 4U,
        RotationZ = 1U << 5U,
        All = 0x3FU
    };

    /** @brief Combines independent axis-lock bits without exposing native solver flags. */
    [[nodiscard]] constexpr PhysicsAxisLock operator|(const PhysicsAxisLock left, const PhysicsAxisLock right) noexcept {
        return static_cast<PhysicsAxisLock>(static_cast<unsigned int>(left) | static_cast<unsigned int>(right));
    }

    /** @brief Intersects independent axis-lock bits without exposing native solver flags. */
    [[nodiscard]] constexpr PhysicsAxisLock operator&(const PhysicsAxisLock left, const PhysicsAxisLock right) noexcept {
        return static_cast<PhysicsAxisLock>(static_cast<unsigned int>(left) & static_cast<unsigned int>(right));
    }

    /** @brief Adds axis-lock bits while preserving the strongly typed public representation. */
    constexpr PhysicsAxisLock &operator|=(PhysicsAxisLock &left, const PhysicsAxisLock right) noexcept {
        left = left | right;
        return left;
    }

    /** @brief Reports whether every requested degree-of-freedom lock is present. */
    [[nodiscard]] constexpr bool HasPhysicsAxisLock(const PhysicsAxisLock value, const PhysicsAxisLock requested) noexcept {
        return (value & requested) == requested;
    }

    /** @brief Frame-rate-independent damping, degree-of-freedom locks and explicit motion ceilings. */
    struct PhysicsMotionSafety final {
        float linearDampingPerSecond{};
        float angularDampingPerSecond{};
        PhysicsAxisLock lockedAxes{PhysicsAxisLock::None};
        float maximumLinearSpeed{static_cast<float>(MaximumPhysicsLinearSpeed)};
        float maximumAngularSpeed{MaximumPhysicsAngularSpeed};
        float maximumDepenetrationSpeed{MaximumPhysicsDepenetrationSpeed};
    };

    /**
     * @brief Portable rigid-body intent without runtime identity, world pose or native state.
     *
     * This value is suitable for ownership by a scene/prefab schema but does not define that
     * schema's component slot, persistence version or body/collider relationship. Initial
     * velocities use m/s and radians/s. It owns no handles, leases or borrowed memory.
     */
    struct PhysicsAuthoredBodyDescriptor final {
        PhysicsMotionType motion{PhysicsMotionType::Static};
        PhysicsMassPolicy mass;
        Math::Vec3 initialLinearVelocity{};
        Math::Vec3 initialAngularVelocity{};
        PhysicsMotionSafety motionSafety;
        PhysicsBodyContinuousCollision continuousCollision;
    };

    /**
     * @brief Common runtime body request referencing an immutable shape in one published world.
     *
     * Owns values only; the shape handle is borrowed and cannot extend a resource lifetime.
     * The pose is in the receiving world's current origin frame. The enclosing owner-thread operation
     * binds/revalidates its origin epoch; this value alone is not a durable queued command.
     * Static requests have no velocity/mass; kinematic requests have no mass; dynamics explicitly
     * select mass or density. Velocity units are m/s and radians/s, with no hidden scale or conversion.
     *
     * This is neither serializable authoring nor a detached scene-plan record. A creation operation
     * additionally requires explicit collider material/filter bindings, admitted shape geometry,
     * capacities and capability evidence; this common descriptor supplies no implicit default filter.
     * Published body identity is issued only after the complete owning transaction succeeds.
     */
    struct PhysicsBodyDescriptor final {
        ShapeHandle shape;
        PhysicsPose pose;
        PhysicsMotionType motion{PhysicsMotionType::Static};
        PhysicsMassPolicy mass;
        Math::Vec3 linearVelocity;
        Math::Vec3 angularVelocity;
        PhysicsMotionSafety motionSafety;
        PhysicsBodyContinuousCollision continuousCollision;
    };

    /** @brief Observable body activity; this is state evidence, not a wake/sleep command. */
    enum class PhysicsBodyActivity : std::uint8_t {
        Awake,
        Sleeping,
        Static /**< Static bodies do not participate in wake/sleep transitions. */
    };

    /**
     * @brief Owned query snapshot of one published body, separate from authored intent and creation policy.
     *
     * The handle identifies the observed body but does not extend its lifetime. Pose and velocities
     * are current world-generation values; activity is observation only. No native solver object,
     * mutable authority, shape ownership or persistence identity is retained.
     */
    struct PhysicsBodyState final {
        BodyHandle body;
        PhysicsPose pose;
        Math::Vec3 linearVelocity{};
        Math::Vec3 angularVelocity{};
        PhysicsBodyActivity activity{PhysicsBodyActivity::Awake};
    };

    /**
     * @brief Checks portable rigid-body intent without requiring runtime identity or a world.
     * @param descriptor Immutable authored motion, mass and initial velocity policy.
     * @return Success, PhysicsErrors::DescriptorInvalid for malformed values or inconsistent
     * motion/mass/velocity policy, or PhysicsErrors::OperationUnsupported for an unknown motion mode.
     * @pre Authoring/conversion use; diagnostics may allocate.
     * @post The input is unchanged. Success does not prove collider, transform, profile or scene admission.
     */
    [[nodiscard]] Result<void> ValidatePhysicsAuthoredBodyDescriptor(const PhysicsAuthoredBodyDescriptor &descriptor);

    /**
     * @brief Resolves portable authored intent into one owned runtime creation request.
     * @param authored Immutable portable intent.
     * @param shape Borrowed immutable shape handle in the expected published world.
     * @param pose Initial body pose in that world's current origin frame.
     * @param expectedWorld Exact published world generation receiving the request.
     * @return The complete runtime descriptor or the stable authored, handle, pose or runtime validation error.
     * @pre Control/owner-thread conversion use; the caller retains any required shape lease.
     * @post Inputs are unchanged. Success publishes no body identity and performs no native work.
     */
    [[nodiscard]] Result<PhysicsBodyDescriptor> ResolvePhysicsBodyDescriptor(const PhysicsAuthoredBodyDescriptor &authored,
                                                                             ShapeHandle shape, PhysicsPose pose,
                                                                             PhysicsWorldId expectedWorld);

    /**
     * @brief Checks common body representation, owner identity, motion/mass consistency and initial velocities.
     * @param descriptor Immutable request in the receiving world's current origin frame.
     * @param expectedWorld Exact published world generation receiving the request.
     * @return Success or a stable Physics handle/world/descriptor/unsupported-operation error.
     * @pre Control/owner-thread use; diagnostics may allocate.
     * @post Success is not a shape lease, native support, geometry/mass derivation or complete creation
     * admission. The world must still resolve current generations, validate shape/motion/scale and
     * local-cluster bounds, material/filter schema and origin epochs, and admit the structural safe point.
     */
    [[nodiscard]] Result<void> ValidatePhysicsBodyDescriptor(const PhysicsBodyDescriptor &descriptor, PhysicsWorldId expectedWorld);

    /**
     * @brief Validates one query-only runtime body snapshot against its expected world generation.
     * @param state Immutable current body state.
     * @param expectedWorld Exact world generation from which the snapshot was read.
     * @return Success or a stable handle, pose, descriptor or unsupported-activity error.
     * @pre Read/query boundary; diagnostics may allocate.
     * @post The state is unchanged. Success grants no mutation authority and extends no body lifetime.
     */
    [[nodiscard]] Result<void> ValidatePhysicsBodyState(const PhysicsBodyState &state, PhysicsWorldId expectedWorld);

    /**
     * @brief Computes the multiplicative exponential damping retained over an exact fixed-step duration.
     * @param dampingPerSecond Non-negative damping coefficient in inverse seconds.
     * @param deltaSeconds Finite non-negative fixed-step duration in seconds.
     * @return Scale in `[0, 1]`, or PhysicsErrors::DescriptorInvalid for malformed values.
     * @post The result depends only on elapsed time, not on how that duration is subdivided into ticks.
     */
    [[nodiscard]] Result<float> ComputePhysicsDampingScale(float dampingPerSecond, double deltaSeconds);
}  // namespace Horo::Physics
