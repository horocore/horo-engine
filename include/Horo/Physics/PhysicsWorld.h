#pragma once

/** @file PhysicsWorld.h
 * @brief Explicit canonical/null Physics runtime ownership and detached world lifecycle.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsCapabilities.h"
#include "Horo/Physics/PhysicsConstraintDescriptor.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsShapeDescriptor.h"
#include "Horo/Physics/PhysicsTickPipeline.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace Horo {
    class JobSystem;
}

namespace Horo::Physics {
    class PhysicsSceneActivationParticipant;

    /** @brief Explicit process composition; headless hosts use Canonical when simulation is required. */
    enum class PhysicsRuntimeMode : std::uint8_t {
        Canonical = 0,
        Null = 1
    };

    /** @brief Observable owner lifecycle; Failed is transient and never returned as a usable runtime. */
    enum class PhysicsRuntimeState : std::uint8_t {
        Ready,
        Failed,
        Stopped
    };

    /** @brief World lifecycle including unpublished candidate, active and terminal states. */
    enum class PhysicsWorldState : std::uint8_t {
        Preparing,
        PreparedSolver,
        PreparedNull,
        ActiveSolver,
        ActiveNull,
        Failed,
        Destroyed
    };

    /** @brief Last owner-driven or fatal lifecycle boundary applied to a world. */
    enum class PhysicsWorldLifecycleCause : std::uint8_t {
        None,
        Reset,
        SceneUnload,
        FatalSolverError,
        ProcessShutdown
    };

    class PhysicsWorld;

    /**
     * @brief One immutable analytic child shape bound to a body-local pose while a scene candidate is staged.
     *
     * The handle is borrowed from the receiving unpublished world. The span passed to compound-shape
     * admission remains caller-owned for the duration of the call; no native pointer or storage is retained.
     */
    struct PhysicsSceneShapeInstance final {
        ShapeHandle shape;
        PhysicsPose localPose;
    };

    /** @brief Complete body request for scene activation, including the uniform sensor policy admitted by the native body. */
    struct PhysicsSceneBodyDescriptor final {
        PhysicsBodyDescriptor body;
        bool sensor{};
    };

    /** @brief Owner-thread reconciliation of retained policy against current native body evidence. */
    struct PhysicsBodyReconciliation final {
        PhysicsBodyDescriptor policy;                                /**< Last applied intent; pose and velocities are not live values. */
        PhysicsBodyState state;                                      /**< Current solver pose, velocity and activity. */
        PhysicsMotionType observedMotion{PhysicsMotionType::Static}; /**< Native motion mode translated to Horo. */
        ShapeHandle observedShape;                                   /**< Resident Horo shape matching the native shape object. */
        std::optional<float> observedMassKilograms;                  /**< Native dynamic mass when translation is unlocked. */
        Math::Vec3 observedBoundsExtent;                             /**< Native broadphase AABB full extents in world units. */
    };

    /**
     * @brief Process-composition owner for canonical native registration or explicit Null behavior.
     *
     * A headless/dedicated host selects Canonical when it requires simulation; Null is an explicit
     * Physics-omitted composition and never an automatic fallback. The runtime and all worlds are
     * owner-thread objects. Worlds retain an internal runtime lease so premature runtime destruction
     * closes admission immediately but cannot tear native globals out from under a surviving world.
     * Expected preparation failures roll back; native heap exhaustion is process-fatal because Jolt
     * cannot unwind allocation failure through its no-exception frames.
     */
    class PhysicsRuntime final {
    public:
        /** @brief Starts the selected process Physics composition transactionally.
         * @param mode Canonical solver or explicit Null/omitted behavior.
         * @param solverJobs Optional injected scheduler that must outlive this runtime and all retained worlds.
         * @return Ready runtime or a typed error after complete partial-startup rollback.
         * @pre Calls are serialized by the process composition root; no foreign Jolt owner is active.
         */
        [[nodiscard]] static Result<std::unique_ptr<PhysicsRuntime>> Create(PhysicsRuntimeMode mode, JobSystem *solverJobs = nullptr);
        /** @brief Closes runtime admission; surviving world leases retain required native globals until retirement. */
        ~PhysicsRuntime();
        PhysicsRuntime(const PhysicsRuntime &) = delete;
        PhysicsRuntime &operator=(const PhysicsRuntime &) = delete;

        /** @brief Creates an unpublished isolated world candidate from one validated snapshot.
         * @param settings Immutable settings copied into the candidate.
         * @return Prepared candidate or a typed error after releasing every acquired world resource.
         * @post No public world identity, body handle, event or command admission is published.
         */
        [[nodiscard]] Result<std::unique_ptr<PhysicsWorld>> PrepareWorld(const PhysicsWorldSettings &settings);

        /** @brief Closes candidate admission and releases native registration after all retained worlds retire; safe repeatedly. */
        void Shutdown() noexcept;
        /** @brief Returns the selected explicit composition. @return Canonical or Null. */
        [[nodiscard]] PhysicsRuntimeMode Mode() const noexcept;
        /** @brief Returns the current owner lifecycle state. @return Ready or Stopped for a successfully created runtime. */
        [[nodiscard]] PhysicsRuntimeState State() const noexcept;
        /** @brief Reports composition availability without inferring it from GUI/headless state.
         * @return Omitted for Null, Available for ready Canonical, Unavailable after Canonical shutdown.
         */
        [[nodiscard]] PhysicsAvailability Availability() const noexcept;
        /** @brief Reports current implemented support; Null reports every known feature Unsupported.
         * @param capability Known Horo feature to inspect.
         * @return WorldCreation, rigid bodies, body mutation, immutable analytic shapes, constraints and immediate queries
         * are available only while Canonical is ready; snapshot, origin-rebasing and other future features remain unsupported.
         */
        [[nodiscard]] PhysicsCapabilitySupport Capability(PhysicsCapability capability) const noexcept;

    private:
        friend class PhysicsWorld;
        friend class PhysicsSceneActivationParticipant;
        struct Impl;

        /** @brief Issues one never-reused process-runtime world identity, consuming it even if later preparation fails. */
        [[nodiscard]] Result<PhysicsWorldId> IssueWorldIdentity();

        /** @brief Retains the successfully prepared process owner. @param impl Owned shared runtime state. */
        explicit PhysicsRuntime(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

        std::shared_ptr<Impl> impl_;
    };

    /** @brief One isolated prepared or active world; owns native state and an immutable settings snapshot. */
    class PhysicsWorld final {
    public:
        /** @brief Releases per-world resources before the retained process-runtime lease. */
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld &) = delete;
        PhysicsWorld &operator=(const PhysicsWorld &) = delete;

        /** @brief Binds the host-issued identity and opens this candidate's lifecycle.
         * @param identity Non-zero process-unique world generation assigned at aggregate activation.
         * @return Success or a typed affinity/state/invalid/duplicate-active identity error; successful binding allocates nothing.
         * @post A successful candidate becomes ActiveSolver or ActiveNull exactly once.
         * The host remains responsible for never reusing a historical process-local generation.
         */
        [[nodiscard]] Result<void> Activate(PhysicsWorldId identity);
        /** @brief Retires active state and rebuilds the unpublished candidate from its immutable settings.
         * @return Success in PreparedSolver/PreparedNull, or a typed affinity/state/reinitialization error.
         * @post A successful reset clears identity, commands, publication and statistics and requires a new Activate call.
         * Calling Reset again while already prepared is a no-op.
         */
        [[nodiscard]] Result<void> Reset();
        /** @brief Closes scene admission and releases all per-world resources on the owner thread.
         * @return Success after complete retirement, including repeated scene-unload calls, or a typed affinity error.
         */
        [[nodiscard]] Result<void> UnloadScene();
        /** @brief Closes admission for process teardown and releases all per-world resources; safe repeatedly. */
        void Shutdown() noexcept;
        /** @brief Reads lifecycle state. @return Current prepared, active or destroyed state. */
        [[nodiscard]] PhysicsWorldState State() const noexcept;
        /** @brief Reads the retained process-local generation. @return Bound identity, or invalid before activation. */
        [[nodiscard]] PhysicsWorldId Identity() const noexcept;
        /** @brief Reads immutable world policy. @return Borrowed snapshot valid for this object's lifetime, including after shutdown. */
        [[nodiscard]] const PhysicsWorldSettings &Settings() const noexcept;
        /** @brief Reads the most recent explicit lifecycle cause. @return None before the first reset, failure or retirement. */
        [[nodiscard]] PhysicsWorldLifecycleCause LifecycleCause() const noexcept;
        /** @brief Reads the retained fatal/reset failure. @return Typed terminal error, or empty outside Failed. */
        [[nodiscard]] const std::optional<Error> &LastFailure() const noexcept;
        /** @brief Reads the latest bounded solver diagnostic retained by this world.
         * @return Owned inert evidence, or empty before a solver finding and after reset/retirement.
         * @note A diagnostic is evidence only. LastFailure and State remain control-flow authority.
         */
        [[nodiscard]] const std::optional<PhysicsDiagnosticRecord> &LastDiagnostic() const noexcept;
        /** @brief Defers one structural intent to its semantic fixed-tick safe point.
         * @param command Owned command envelope copied into bounded world storage.
         * @return Admission status, or a typed malformed/state/affinity error. Rejected work remains caller-owned.
         * Destruction may consume the reserved final slot; if completely full it returns DestructionRetryRequired
         * and is never silently dropped. Commands carry their exact future tick and are canonically sorted at that tick;
         * admission or worker completion order has no semantic authority.
         * A Change/Body command may own a PhysicsBodyMutation. It is validated against the resident
         * body on admission between ticks, applied before the native step, and revalidated at that safe point.
         */
        [[nodiscard]] Result<PhysicsCommandAdmission> QueueStructuralCommand(const PhysicsStructuralCommand &command);
        /** @brief Reads the last applied policy for a resident scene body on the owner thread.
         * @param body Exact current world-scoped body handle.
         * @return Owned descriptor reflecting completed body mutations, or a typed lifecycle/handle error.
         * The pose and velocities in this policy are creation intent, not a live solver-state snapshot.
         */
        [[nodiscard]] Result<PhysicsBodyDescriptor> ReadSceneBodyPolicy(BodyHandle body) const;
        /** @brief Reconciles one exact resident body against owner-thread native solver state.
         * @param body Exact current world-scoped body handle.
         * @return Owned policy and observed state, or a typed lifecycle, handle or native-consistency error.
         * @pre Owner thread, active canonical world, outside a fixed step. This is not a cross-thread snapshot.
         */
        [[nodiscard]] Result<PhysicsBodyReconciliation> ReadSceneBodyReconciliation(BodyHandle body) const;
        /**
         * @brief Admits one explicit analytic query fixture on the owner thread.
         * @param fixture Complete geometry, pose and stable query-filter evidence.
         * @return Generation-safe body and shape identities, or a typed admission error.
         * @pre Active canonical world, outside a fixed-step execution.
         * @post The returned identities remain valid until DestroyQueryFixture, reset, unload or shutdown.
         */
        [[nodiscard]] Result<PhysicsQueryFixture> CreateQueryFixture(const PhysicsQueryFixtureDescriptor &fixture) const;
        /**
         * @brief Retires one exact query fixture on the owner thread.
         * @param fixture Body and shape identities returned by CreateQueryFixture.
         * @return Success or a typed malformed, foreign-world, stale or lifecycle error.
         */
        [[nodiscard]] Result<void> DestroyQueryFixture(const PhysicsQueryFixture &fixture) const;
        /**
         * @brief Stages one validated analytic shape in the active scene candidate.
         * @param descriptor Scale-free analytic geometry in canonical SI units.
         * @return World-scoped shape identity or a typed validation/capacity/native error.
         * @pre Active canonical world, owner-thread scene preparation, and no fixed-step execution.
         * @post The shape is private to this world generation and is destroyed by reset, unload or shutdown.
         */
        [[nodiscard]] Result<ShapeHandle> CreateSceneShape(const PhysicsShapeDescriptor &descriptor) const;
        /**
         * @brief Stages one immutable compound shape from already admitted child shapes.
         * @param instances Caller-owned child shape identities and body-local poses.
         * @return World-scoped compound shape identity or a typed validation/capacity/native error.
         * @pre Every child belongs to this active world generation and the span remains valid for the call.
         * @post No identity escapes until the enclosing scene activation candidate succeeds.
         */
        [[nodiscard]] Result<ShapeHandle> CreateSceneCompoundShape(std::span<const PhysicsSceneShapeInstance> instances) const;
        /**
         * @brief Stages one native body against an already admitted scene shape.
         * @param descriptor Body policy, initial pose, shape identity and sensor policy.
         * @return World-scoped body identity or a typed validation/capacity/native error.
         * @pre Active canonical world, owner-thread scene preparation, and no fixed-step execution.
         * @post Partial native state remains owned by this world and is released on any later activation failure.
         */
        [[nodiscard]] Result<BodyHandle> CreateSceneBody(const PhysicsSceneBodyDescriptor &descriptor) const;
        /**
         * @brief Stages one fixed, distance, hinge or slider constraint after its body endpoints are resident.
         * @param descriptor World-scoped body anchors and typed constraint policy.
         * @return World-scoped constraint identity or a typed validation/capacity/native error.
         * @pre Active canonical world, owner-thread scene preparation, and every body endpoint is resident.
         * @post Constraint ownership remains private to this world until aggregate publication.
         */
        [[nodiscard]] Result<ConstraintHandle> CreateSceneConstraint(const PhysicsConstraintDescriptor &descriptor) const;
        /**
         * @brief Removes one exact resident joint before retiring either endpoint body.
         * @param constraint Generation-scoped identity returned by CreateSceneConstraint.
         * @return Success or typed affinity, lifecycle, foreign-world or stale-handle error.
         * @pre Active canonical world on its owner thread, outside a fixed tick.
         * @post Native solver ownership and collision policy are removed; repeated destruction is stale.
         */
        [[nodiscard]] Result<void> DestroySceneConstraint(ConstraintHandle constraint) const;
        /**
         * @brief Reads the current signed coordinate of one resident hinge or slider joint.
         * @param constraint Exact generation-scoped joint identity.
         * @return Angle in radians or displacement in meters; typed lifecycle, affinity, stale-handle or
         * OperationUnsupported error for a fixed/distance joint.
         * @pre Active canonical world on its owner thread, outside a fixed tick. This copy retains no joint lease.
         */
        [[nodiscard]] Result<PhysicsJointState> ReadSceneJointState(ConstraintHandle constraint) const;
        /**
         * @brief Executes one immediate query against the current owner-thread broadphase.
         * @param descriptor Exact world/scene query request.
         * @param hits Caller-owned bounded hit storage; no world or native lifetime is retained.
         * @return Bounded result metadata, including explicit caller-storage truncation.
         * @pre Active canonical world, outside a fixed-step execution, and owner-thread affinity.
         */
        [[nodiscard]] Result<PhysicsQueryResult> Query(const PhysicsQueryDescriptor &descriptor, std::span<PhysicsQueryHit> hits) const;
        /** @brief Executes one exact host-issued fixed tick and publishes its results atomically.
         * @param input One-based next tick, exact immutable world delta and optional synchronous observer.
         * @return Success or typed affinity/lifecycle/sequence/delta/job/native-capacity error without partial publication.
         * @pre Active canonical world on its owner thread; reentrant stepping is rejected.
         * Every admitted solver job is joined before native integration or publication. A child failure makes
         * the world Failed exactly once; deadline expiry triggers cooperative cancellation and drains all accepted work.
         */
        [[nodiscard]] Result<void> AdvanceFixedTick(const PhysicsFixedTickInput &input);
        /** @brief Reads one coherent copy of the last atomically completed tick marker from any thread.
         * @return Zero revision before the first completed tick.
         * @pre The caller keeps this PhysicsWorld alive for the complete call; the snapshot lock does not extend object lifetime.
         */
        [[nodiscard]] PhysicsPublishedTick PublishedTick() const noexcept;
        /** @brief Reads allocation-free cumulative pipeline metrics. @return Owner-thread value snapshot. */
        [[nodiscard]] PhysicsTickStatistics TickStatistics() const noexcept;

    private:
        friend class PhysicsRuntime;
        struct Impl;
        /** @brief Takes one prepared world's ownership. @param impl Owned isolated world state. */
        explicit PhysicsWorld(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Physics
