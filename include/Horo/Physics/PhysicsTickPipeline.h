#pragma once

/** @file PhysicsTickPipeline.h
 * @brief Fixed-tick phase, deferred structural command and atomic publication contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Time.h"
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsDeterminismPolicy.h"

#include <cstdint>
#include <optional>

namespace Horo::Physics {
    /** @brief Hard admission bound for optional solver-neutral child work in one fixed tick. */
    inline constexpr std::uint32_t MaximumPhysicsSolverJobsPerTick = 1'024;

    /** @brief One solver-neutral child operation whose borrowed context remains valid through the enclosing fixed tick. */
    struct PhysicsSolverJob final {
        void *context{}; /**< Caller-owned immutable or privately synchronized state; never a PhysicsWorld reference. */
        Result<void> (*execute)(void *context, const CancellationToken &cancellation) noexcept {};
        /**< Required non-throwing operation; cancellation must be observed cooperatively. */
    };

    /** @brief Optional bounded child-work batch dispatched and joined as part of one fixed tick. */
    struct PhysicsSolverJobBatch final {
        const PhysicsSolverJob *jobs{}; /**< Borrowed contiguous jobs valid until AdvanceFixedTick returns. */
        std::uint32_t jobCount{};       /**< Number of jobs up to MaximumPhysicsSolverJobsPerTick; zero disables dispatch. */
        Duration joinTimeout{};         /**< Positive shared deadline for the complete batch. */
    };

    /** @brief Ordered stages of one canonical Physics fixed tick. */
    enum class PhysicsTickPhase : std::uint8_t {
        ApplyDeferredPreStep,
        CopyKinematicTargets,
        ApplyDynamicInputs,
        BroadPhase,
        ContactGeneration,
        ConstraintSolve,
        IntegrateBodies,
        WriteRuntimeTransforms,
        ProduceEvents,
        ApplyDeferredPostStep,
        PublishCompletedTick
    };

    /** @brief Safe point selected from command semantics, never from producer timing. */
    enum class PhysicsCommandSafePoint : std::uint8_t {
        PreStep,
        PostStep
    };

    /** @brief Explicit activation policy after a body change; shape or mode changes always wake moving bodies. */
    enum class PhysicsBodyWakePolicy : std::uint8_t {
        Preserve,
        Wake
    };

    /**
     * @brief Owned partial replacement of one resident scene body at the pre-step safe point.
     *
     * Absent fields retain their current policy and velocity. Shape and mode changes rebuild the
     * broadphase/contact representation in place and wake moving bodies. No pose or native ID is
     * accepted; the body retains its current solver pose and Horo handle. A constrained body cannot
     * be changed until a qualified constraint-reconciliation path exists.
     */
    struct PhysicsBodyMutation final {
        BodyHandle body;
        std::optional<ShapeHandle> shape;
        std::optional<PhysicsMotionType> motion;
        std::optional<PhysicsMassPolicy> mass;
        std::optional<PhysicsMotionSafety> motionSafety;
        std::optional<Math::Vec3> linearVelocity;
        std::optional<Math::Vec3> angularVelocity;
        PhysicsBodyWakePolicy wake{PhysicsBodyWakePolicy::Preserve}; /**< Explicit override for otherwise non-waking damping edits. */
    };

    /**
     * @brief Bounded structural-command envelope retained by value until one fixed-tick safe point.
     *
     * The complete key is producer-assigned deterministic evidence. Admission order is deliberately
     * irrelevant: Physics sorts the bounded eligible frame by the canonical version-one comparator.
     * Create/change apply before the solver step and destruction applies after all solver work completes
     * but before publication.
     */
    struct PhysicsStructuralCommand final {
        PhysicsCommandOrderKey order;                    /**< Complete tick/world/scene/target/source canonical key. */
        std::optional<PhysicsBodyMutation> bodyMutation; /**< Only Change/Body commands may carry a native mutation. */
    };

    /** @brief Non-throwing command admission result; rejected work remains owned by the caller. */
    enum class PhysicsCommandAdmissionStatus : std::uint8_t {
        Deferred,
        RejectedFull,
        DestructionRetryRequired
    };

    /** @brief Admission result with queue depth after the attempt. */
    struct PhysicsCommandAdmission final {
        PhysicsCommandAdmissionStatus status{PhysicsCommandAdmissionStatus::RejectedFull}; /**< Ownership outcome. */
        std::uint32_t pendingCommands{};                                                   /**< Depth after this attempt. */
    };

    /**
     * @brief Optional allocation-free observation hook for tests and bounded diagnostics.
     *
     * Callbacks run synchronously on the Physics owner thread. They may enqueue more commands; those
     * commands are retained for the next tick and cannot enter the command batch currently executing.
     * QueueStructuralCommand is the only world mutation permitted from a callback; every other solver
     * or scene mutation is forbidden. Callbacks must not throw, retain borrowed command references or
     * call AdvanceFixedTick reentrantly.
     */
    struct PhysicsTickObserver final {
        void *context{}; /**< Caller-owned context valid through AdvanceFixedTick. */
        void (*phase)(void *context, PhysicsTickPhase phase, std::uint64_t tick) noexcept {}; /**< Optional phase observation. */
        void (*command)(void *context, const PhysicsStructuralCommand &command, PhysicsCommandSafePoint safePoint,
                        std::uint64_t tick) noexcept {}; /**< Optional safe-point command observation. */
    };

    /** @brief Exact host-owned fixed-tick attempt; Physics never samples or accumulates wall time. */
    struct PhysicsFixedTickInput final {
        std::uint64_t simulationTick{};   /**< One-based next tick supplied by Runtime. */
        std::uint64_t sceneGeneration{};  /**< Non-zero scene generation owning this exact command frame. */
        Duration fixedDelta{};            /**< Exact configured host quantum, never a measured frame delta. */
        PhysicsTickObserver observer;     /**< Optional synchronous observation only. */
        PhysicsSolverJobBatch solverJobs; /**< Optional child work joined before native integration and publication. */
    };

    /** @brief Atomically replaced publication marker for transforms, queries and events from one completed tick. */
    struct PhysicsPublishedTick final {
        std::uint64_t completedTick{};       /**< Last successfully completed simulation tick. */
        std::uint64_t publicationRevision{}; /**< Monotonic all-domain publication revision. */
        std::uint64_t transformTick{};       /**< Tick owning the visible transform snapshot. */
        std::uint64_t queryTick{};           /**< Tick owning the visible query snapshot. */
        std::uint64_t eventTick{};           /**< Tick owning the visible event batch. */
        std::uint32_t appliedCommands{};     /**< Eligible commands processed at this tick's safe points. */
        std::uint32_t eventCount{};          /**< Number of immutable contact/trigger records in the event batch. */
        std::uint64_t droppedEventCount{};   /**< Records omitted while capturing or publishing this tick. */
    };

    /** @brief Allocation-free cumulative fixed-tick and command-buffer metrics. */
    struct PhysicsTickStatistics final {
        std::uint64_t completedTicks{};        /**< Successfully published ticks. */
        std::uint64_t admittedCommands{};      /**< Commands copied into bounded storage. */
        std::uint64_t rejectedCommands{};      /**< Full-buffer admissions retaining caller ownership. */
        std::uint64_t destructionRetryCount{}; /**< Destructions returned for mandatory retry. */
        std::uint64_t droppedEventCount{};     /**< Cumulative copied or projected records omitted by policy. */
        std::uint32_t pendingCommands{};       /**< Current deferred depth. */
        std::uint32_t maximumCommandDepth{};   /**< High-water mark since world preparation. */
        std::uint32_t eventDepth{};            /**< Number of records in the last published event batch. */
        std::uint32_t maximumEventDepth{};     /**< High-water mark of one published event batch. */
    };
}  // namespace Horo::Physics
