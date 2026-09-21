#pragma once

/** @file PhysicsTransformAuthority.h
 * @brief Typed transform-authority commands and safe-phase publication for Physics bodies.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsDeterminismPolicy.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <variant>

namespace Horo::Physics {
    /** @brief Version of the transform-authority command and snapshot contract. */
    inline constexpr std::uint32_t PhysicsTransformAuthorityProtocolVersion = 1;
    /** @brief Maximum body records reserved by one transform-authority candidate. */
    inline constexpr std::uint32_t MaximumPhysicsTransformAuthorityBodies = MaximumPhysicsResourceRecords;
    /** @brief Maximum deferred transform commands retained by one authority. */
    inline constexpr std::uint32_t MaximumPhysicsTransformAuthorityCommands = MaximumPhysicsBufferEntries;

    /** @brief Identifies the sole source allowed to drive a body's runtime transform. */
    enum class PhysicsTransformAuthority : std::uint8_t {
        StaticScene,
        KinematicTarget,
        DynamicSolver
    };

    /** @brief Selects the safe static representation update after a scene pose changes. */
    enum class PhysicsStaticTransformUpdatePolicy : std::uint8_t {
        UpdateBroadphase,
        Rebuild
    };

    /** @brief Explicit dynamic transform control that is allowed to cross the host/solver boundary. */
    enum class PhysicsDynamicTransformOperation : std::uint8_t {
        Teleport,
        Reset
    };

    /** @brief Selects whether a dynamic teleport retains or clears solver velocity state. */
    enum class PhysicsTeleportVelocityPolicy : std::uint8_t {
        Preserve,
        Reset
    };

    /** @brief Identifies the transform command target and its deterministic consuming tick. */
    struct PhysicsTransformCommandIdentity final {
        std::uint32_t protocolVersion{PhysicsTransformAuthorityProtocolVersion}; /**< Exact supported protocol. */
        std::uint64_t simulationTick{};                                          /**< One-based consuming tick. */
        std::uint64_t sceneGeneration{};                                         /**< Exact owning scene generation. */
        BodyHandle body;                                                         /**< Generation-checked body target. */
        PhysicsCommandSourceId source;                                           /**< Stable producer authority. */
        std::uint64_t sourceSequence{};                                          /**< Non-zero source-owned order position. */

        [[nodiscard]] constexpr auto operator<=>(const PhysicsTransformCommandIdentity &) const noexcept = default;
    };

    /** @brief Static scene-authority change applied before broadphase work begins. */
    struct PhysicsStaticTransformCommand final {
        PhysicsTransformCommandIdentity identity;
        PhysicsPose authoredPose; /**< Caller-owned scene value copied into the detached runtime plan. */
        PhysicsStaticTransformUpdatePolicy updatePolicy{PhysicsStaticTransformUpdatePolicy::UpdateBroadphase};
    };

    /** @brief Kinematic target copied exactly once during the named fixed-tick pre-step phase. */
    struct PhysicsKinematicTargetCommand final {
        PhysicsTransformCommandIdentity identity;
        PhysicsPose targetPose; /**< Target in the active Physics origin frame; never a render-time sample. */
    };

    /** @brief Explicit dynamic teleport/reset control applied before the consuming solver step. */
    struct PhysicsDynamicTransformCommand final {
        PhysicsTransformCommandIdentity identity;
        PhysicsPose targetPose;
        PhysicsDynamicTransformOperation operation{PhysicsDynamicTransformOperation::Teleport};
        PhysicsTeleportVelocityPolicy velocityPolicy{PhysicsTeleportVelocityPolicy::Preserve};
    };

    /** @brief Closed typed set of transform mutations admitted by the authority boundary. */
    using PhysicsTransformCommand =
        std::variant<PhysicsStaticTransformCommand, PhysicsKinematicTargetCommand, PhysicsDynamicTransformCommand>;

    /** @brief Completed-tick solver output; it is observation and writeback evidence, never authored input. */
    struct PhysicsDynamicTransformSnapshot final {
        std::uint32_t protocolVersion{PhysicsTransformAuthorityProtocolVersion}; /**< Exact supported protocol. */
        std::uint64_t completedTick{};                                           /**< Tick that fully produced this state. */
        std::uint64_t sceneGeneration{};                                         /**< Exact owning scene generation. */
        PhysicsBodyState state;                                                  /**< Copied solver result for one dynamic body. */
    };

    /** @brief Direct host transform write used to report an authority violation before any mutation. */
    struct PhysicsDirectTransformWrite final {
        std::uint32_t protocolVersion{PhysicsTransformAuthorityProtocolVersion}; /**< Exact supported protocol. */
        std::uint64_t sceneGeneration{};                                         /**< Exact owning scene generation. */
        BodyHandle body;                                                         /**< Generation-checked body target. */
        PhysicsPose pose;                                                        /**< Candidate pose, never applied by this operation. */
    };

    /** @brief Body transform input installed in a detached candidate before activation. */
    struct PhysicsBodyTransformRegistration final {
        BodyHandle body;                                     /**< Stable body identity owned by the enclosing scene plan. */
        PhysicsMotionType motion{PhysicsMotionType::Static}; /**< Fixed transform-authority mode. */
        PhysicsPose authoredPose;                            /**< Copied authored seed; no document pointer is retained. */
    };

    /** @brief Owned transform evidence returned without exposing mutable body storage. */
    struct PhysicsBodyTransformState final {
        BodyHandle body;
        PhysicsMotionType motion{PhysicsMotionType::Static};
        PhysicsPose authoredPose;          /**< The detached authored seed; dynamic publication never changes it. */
        PhysicsPose runtimePose;           /**< Current safe-phase runtime pose. */
        std::uint64_t lastAppliedTick{};   /**< Last pre-step tick applied to this body. */
        std::uint64_t lastPublishedTick{}; /**< Last completed dynamic snapshot tick, or zero when absent. */
        bool hasPublishedSnapshot{};       /**< Whether lastPublishedTick names a readable dynamic snapshot. */
    };

    /** @brief Per-tick counts proving which authority operations were applied at the pre-step boundary. */
    struct PhysicsTransformTickResult final {
        std::uint64_t simulationTick{};
        std::uint32_t appliedCommands{};
        std::uint32_t staticBroadphaseUpdates{};
        std::uint32_t staticRebuilds{};
        std::uint32_t kinematicTargets{};
        std::uint32_t dynamicControls{};
    };

    /** @brief Non-blocking result of copying one transform command into bounded authority storage. */
    enum class PhysicsTransformCommandAdmissionStatus : std::uint8_t {
        Deferred,
        RejectedFull
    };

    /** @brief Admission status and queue depth after one transform-command attempt. */
    struct PhysicsTransformCommandAdmission final {
        PhysicsTransformCommandAdmissionStatus status{PhysicsTransformCommandAdmissionStatus::RejectedFull};
        std::uint32_t pendingCommands{};
    };

    /** @brief Immutable owner generations and reservations for one detached authority candidate. */
    struct PhysicsTransformAuthorityDescriptor final {
        PhysicsWorldId world;
        std::uint64_t sceneGeneration{};
        std::uint32_t maximumBodies{1};
        std::uint32_t maximumPendingCommands{1};
    };

    /** @brief Lifecycle of a transform-authority candidate. */
    enum class PhysicsTransformAuthorityState : std::uint8_t {
        Prepared,
        Active,
        Destroyed
    };

    /**
     * @brief Resolves the transform authority assigned by the three Physics motion modes.
     * @param motion Static, kinematic or dynamic body mode.
     * @return StaticScene, KinematicTarget or DynamicSolver, or OperationUnsupported for an unknown mode.
     */
    [[nodiscard]] Result<PhysicsTransformAuthority> ResolvePhysicsTransformAuthority(PhysicsMotionType motion);

    /**
     * @brief Validates command identity without consulting body liveness.
     * @param identity Candidate target, scene, tick and source evidence.
     * @param expectedWorld Active world generation receiving the command.
     * @param expectedSceneGeneration Active scene generation receiving the command.
     * @param expectedSimulationTick Tick selected for admission.
     * @return Success or a stable world, handle, order or stale-generation error.
     */
    [[nodiscard]] Result<void> ValidatePhysicsTransformCommandIdentity(const PhysicsTransformCommandIdentity &identity,
                                                                       PhysicsWorldId expectedWorld, std::uint64_t expectedSceneGeneration,
                                                                       std::uint64_t expectedSimulationTick);

    /**
     * @brief Validates a complete static transform update before deferred admission.
     * @param command Static authored pose and safe update policy.
     * @param expectedWorld Active receiving world.
     * @param expectedSceneGeneration Active receiving scene.
     * @param expectedSimulationTick Exact consuming tick.
     * @return Success or a stable authority, descriptor or ordering error.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStaticTransformCommand(const PhysicsStaticTransformCommand &command,
                                                                     PhysicsWorldId expectedWorld, std::uint64_t expectedSceneGeneration,
                                                                     std::uint64_t expectedSimulationTick);

    /**
     * @brief Validates a complete kinematic target before deferred admission.
     * @param command Tick-addressed target pose and producer identity.
     * @param expectedWorld Active receiving world.
     * @param expectedSceneGeneration Active receiving scene.
     * @param expectedSimulationTick Exact consuming tick.
     * @return Success or a stable authority, descriptor or ordering error.
     */
    [[nodiscard]] Result<void> ValidatePhysicsKinematicTargetCommand(const PhysicsKinematicTargetCommand &command,
                                                                     PhysicsWorldId expectedWorld, std::uint64_t expectedSceneGeneration,
                                                                     std::uint64_t expectedSimulationTick);

    /**
     * @brief Validates explicit dynamic teleport/reset control before deferred admission.
     * @param command Explicit target pose and velocity/reset policy.
     * @param expectedWorld Active receiving world.
     * @param expectedSceneGeneration Active receiving scene.
     * @param expectedSimulationTick Exact consuming tick.
     * @return Success or a stable authority, descriptor or ordering error.
     */
    [[nodiscard]] Result<void> ValidatePhysicsDynamicTransformCommand(const PhysicsDynamicTransformCommand &command,
                                                                      PhysicsWorldId expectedWorld, std::uint64_t expectedSceneGeneration,
                                                                      std::uint64_t expectedSimulationTick);

    /**
     * @brief Validates one completed-tick dynamic snapshot without mutating any state.
     * @param snapshot Copied solver output.
     * @param expectedWorld Active receiving world.
     * @param expectedSceneGeneration Active scene generation.
     * @param expectedCompletedTick Tick whose publication boundary is open.
     * @return Success or a stable descriptor, handle, stale-generation or ordering error.
     */
    [[nodiscard]] Result<void> ValidatePhysicsDynamicTransformSnapshot(const PhysicsDynamicTransformSnapshot &snapshot,
                                                                       PhysicsWorldId expectedWorld, std::uint64_t expectedSceneGeneration,
                                                                       std::uint64_t expectedCompletedTick);

    /**
     * @brief Validates the direct-write evidence used to return an authority diagnostic.
     * @param write Candidate host write; it is never applied by validation.
     * @param expectedWorld Active receiving world.
     * @param expectedSceneGeneration Active scene generation.
     * @return Success for representation only; the authority owner still rejects the operation.
     */
    [[nodiscard]] Result<void> ValidatePhysicsDirectTransformWrite(const PhysicsDirectTransformWrite &write, PhysicsWorldId expectedWorld,
                                                                   std::uint64_t expectedSceneGeneration);

    /**
     * @brief Orders transform commands by tick, body identity, semantic kind and producer evidence.
     * @param left First command.
     * @param right Second command.
     * @return True when left precedes right in the canonical version-one order.
     */
    [[nodiscard]] bool PhysicsTransformCommandLess(const PhysicsTransformCommand &left, const PhysicsTransformCommand &right) noexcept;

    /**
     * @brief Owns detached body transform authority and safe-phase publication state.
     *
     * Body records are prepared before activation and contain copied authored values only. Static
     * updates and kinematic targets are consumed at ApplyPreStep; dynamic state changes only through
     * completed snapshots or explicit teleport/reset commands. The class owns no scene document,
     * native solver object, render state or borrowed memory.
     */
    class PhysicsBodyTransformAuthority final {
    public:
        /**
         * @brief Allocates a detached bounded authority candidate on the calling owner thread.
         * @param descriptor World/scene generations and complete body/command reservations.
         * @return Prepared candidate or a typed descriptor/capacity error without publication.
         */
        [[nodiscard]] static Result<std::unique_ptr<PhysicsBodyTransformAuthority>> Prepare(
            const PhysicsTransformAuthorityDescriptor &descriptor);

        /** @brief Destroys the candidate after closing admission and clearing detached state. */
        ~PhysicsBodyTransformAuthority();
        PhysicsBodyTransformAuthority(const PhysicsBodyTransformAuthority &) = delete;
        PhysicsBodyTransformAuthority &operator=(const PhysicsBodyTransformAuthority &) = delete;

        /** @brief Publishes prepared body records as an active authority without allocating. */
        [[nodiscard]] Result<void> Activate();

        /**
         * @brief Installs one body record before activation.
         * @param registration Stable body identity, fixed motion mode and copied authored seed.
         * @return Success or a typed owner, descriptor, duplicate or capacity error.
         */
        [[nodiscard]] Result<void> RegisterBody(const PhysicsBodyTransformRegistration &registration);

        /**
         * @brief Removes one prepared body record without touching an active solver.
         * @param body Exact registered handle.
         * @return Success or a typed owner, lifecycle or stale-handle error.
         */
        [[nodiscard]] Result<void> UnregisterBody(const BodyHandle &body);

        /**
         * @brief Copies one static, kinematic or explicit dynamic-control command into bounded storage.
         * @param command Owned command value; no producer memory is retained.
         * @return Deferred, full, or a typed authority/order/lifecycle error.
         */
        [[nodiscard]] Result<PhysicsTransformCommandAdmission> QueueTransformCommand(const PhysicsTransformCommand &command);

        /**
         * @brief Applies exactly the selected tick's commands at the pre-step safe point.
         * @param simulationTick One-based next tick; every tick is consumed once even when empty.
         * @return Counts of applied safe-phase operations, or a typed lifecycle/order error.
         */
        [[nodiscard]] Result<PhysicsTransformTickResult> ApplyPreStep(std::uint64_t simulationTick);

        /**
         * @brief Publishes one complete dynamic solver result after the consuming tick finishes.
         * @param snapshot Completed-tick dynamic state copied by value.
         * @return Success or a typed authority/stale/order/lifecycle error; authoredPose is untouched.
         */
        [[nodiscard]] Result<void> PublishDynamicSnapshot(const PhysicsDynamicTransformSnapshot &snapshot);

        /**
         * @brief Rejects a direct host write with a stable authority diagnostic before mutation.
         * @param write Candidate host transform write.
         * @return Always TransformAuthorityViolation after representation/liveness checks for an active body.
         */
        [[nodiscard]] Result<void> WriteHostTransform(const PhysicsDirectTransformWrite &write);

        /**
         * @brief Copies one body's authored/runtime transform evidence.
         * @param body Exact registered body handle.
         * @return Owned evidence or a typed owner/lifecycle/stale-handle error.
         */
        [[nodiscard]] Result<PhysicsBodyTransformState> BodyTransform(const BodyHandle &body) const;

        /**
         * @brief Copies the last completed dynamic state for one body.
         * @param body Exact registered dynamic body handle.
         * @return Snapshot or QuerySnapshotStale before its first completed publication.
         */
        [[nodiscard]] Result<PhysicsBodyState> DynamicSnapshot(const BodyHandle &body) const;

        /** @brief Closes admission and clears all detached records; safe to call repeatedly. */
        void Shutdown() noexcept;
        /** @brief Returns current owner-thread lifecycle state. */
        [[nodiscard]] PhysicsTransformAuthorityState State() const noexcept;
        /** @brief Returns immutable owner generations and reservations. */
        [[nodiscard]] const PhysicsTransformAuthorityDescriptor &Descriptor() const noexcept;
        /** @brief Returns current deferred command count. */
        [[nodiscard]] std::uint32_t PendingCommandCount() const noexcept;

    private:
        struct Impl;
        /** @brief Takes ownership of a fully prepared implementation. */
        explicit PhysicsBodyTransformAuthority(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Physics
