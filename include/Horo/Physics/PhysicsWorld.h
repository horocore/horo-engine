#pragma once

/** @file PhysicsWorld.h
 * @brief Explicit canonical/null Physics runtime ownership and detached world lifecycle.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsCapabilities.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsTickPipeline.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <cstdint>
#include <memory>
#include <optional>
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
         * @return WorldCreation is available only while Canonical is ready; later body/query features remain unsupported.
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
         */
        [[nodiscard]] Result<PhysicsCommandAdmission> QueueStructuralCommand(const PhysicsStructuralCommand &command);
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
