#pragma once

/** @file SaveCaptureBarrier.h
 * @brief Nonblocking owner-thread quiescence authority and bounded immutable capture handoff.
 */

#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveSafePointCoordinator.h"

#include <thread>
#include <vector>

namespace Horo::Runtime {
    /** @brief Independently required runtime authorities at a coherent save cut. */
    enum class SaveBarrierDomain : std::uint8_t {
        FixedSimulation,
        Jobs,
        SceneMutation,
        Subsystem
    };
    /** @brief Trusted inability to publish a coherent participant version. */
    enum class SaveBarrierDenial : std::uint8_t {
        None,
        VersionUnavailable,
        MutationCannotQuiesce,
        CapacityUnavailable
    };
    /** @brief Project/host choice when quiescence is denied or exceeds its deadline. */
    enum class SaveBarrierFailurePolicy : std::uint8_t {
        Defer,
        Fail
    };
    /** @brief Bounded nonblocking request state, retained until explicit acknowledgement. */
    enum class SaveBarrierState : std::uint8_t {
        Idle,
        Pending,
        Captured,
        Deferred,
        Failed,
        Cancelled
    };
    /** @brief Exact observed cause; no participant address crosses the handoff. */
    enum class SaveBarrierReason : std::uint8_t {
        None,
        Mutating,
        EpochUnavailable,
        Denied,
        Timeout,
        CaptureBudget,
        CaptureFailure
    };

    /** @brief Finite limits for one request; synchronous capture never waits for worker I/O. */
    struct SaveCaptureBarrierPolicy final {
        Duration quiesceTimeout{Duration::FromMilliseconds(100)};
        Duration captureBudget{Duration::FromMilliseconds(5)};
        SaveBarrierFailurePolicy failure{SaveBarrierFailurePolicy::Defer};
    };

    /** @brief Exact fence issued by one authority; old tickets cannot finish newer mutations. */
    struct SaveBarrierMutation final {
        std::uint64_t authority{};
        std::size_t participant{};
        std::uint64_t serial{};
        [[nodiscard]] auto operator<=>(const SaveBarrierMutation &) const noexcept = default;
    };

    /** @brief Immutable request/readiness/timing evidence; all access is owner-thread restricted. */
    struct SaveCaptureBarrierSnapshot final {
        OperationId operation{};
        SaveRuntimeGeneration generation;
        SaveBarrierState state{SaveBarrierState::Idle};
        SaveBarrierReason reason{SaveBarrierReason::None};
        std::optional<std::size_t> participant;
        SaveBarrierDenial denial{SaveBarrierDenial::None};
        CanonicalCaptureEpoch epoch;
        Duration elapsed;
        Duration captureDuration;
        std::uint64_t revision{};
    };

    /** @brief Nonblocking poll outcome; only Captured contains a detached snapshot. */
    struct SaveCaptureBarrierOutcome final {
        SaveCaptureBarrierSnapshot barrier;
        std::optional<RuntimeSaveSnapshot> capture;
        std::optional<Error> error;
    };

    /**
     * @brief Session-owned authority for participant mutation and epoch-qualified safe-point capture.
     *
     * Every producer must acquire a mutation ticket before changing canonical state, finish it only
     * after its jobs/structural writes commit, and publish the resulting semantic epoch. Jobs publish
     * completion through their host on this owner thread. A pending request permits normal simulation;
     * only the synchronous capture call closes mutation admission. No lock or pause survives handoff.
     * The host registers all four domains and every registry capture owner before requesting a save.
     */
    class SaveCaptureBarrier final {
    public:
        /** @brief Creates a fixed-capacity authority bound to this thread and an injected monotonic clock.
         * @param authority Non-zero session-unique identity, never reused while tickets survive.
         * @param clock Clock that outlives this authority.
         * @param maximumParticipants Positive capacity, at most 256.
         * @param policy Positive timeout/budget within one minute and known failure policy.
         * @return Authority or a typed invalid/allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<SaveCaptureBarrier>> Create(std::uint64_t authority, Clock &clock,
                                                                                std::size_t maximumParticipants,
                                                                                SaveCaptureBarrierPolicy policy = {});
        SaveCaptureBarrier(const SaveCaptureBarrier &) = delete;
        SaveCaptureBarrier &operator=(const SaveCaptureBarrier &) = delete;

    private:
        /** @brief Factory-issued construction capability; external callers cannot create a key. */
        class ConstructionKey {
            ConstructionKey() = default;
            friend class SaveCaptureBarrier;
        };

    public:
        /** @brief Internal factory-only construction after policy validation.
         * @param authority Validated session identity. @param clock Host clock lease.
         * @param capacity Validated fixed participant capacity. @param policy Validated timing/failure policy.
         * @param key Private construction capability issued only by Create.
         */
        SaveCaptureBarrier(std::uint64_t authority, Clock &clock, std::size_t capacity, SaveCaptureBarrierPolicy policy,
                           ConstructionKey key);

        /** @brief Registers one stable semantic owner before requests start.
         * @param participant Unique canonical participant identity (including host-only domain sentinels).
         * @param domain Required authority domain. @return Stable index or typed invalid/capacity/lifecycle error.
         */
        [[nodiscard]] Result<std::size_t> Register(SaveParticipantId participant, SaveBarrierDomain domain);
        /** @brief Acquires exclusive mutation ownership and invalidates prior readiness.
         * @param participant Registered index. @return Exact ticket or typed affinity/state error.
         */
        [[nodiscard]] Result<SaveBarrierMutation> BeginMutation(std::size_t participant);
        /** @brief Commits one exact mutation and its coherent semantic version.
         * @param mutation Exact unspent ticket. @param epoch Non-zero committed simulation epoch.
         * @return Success or typed stale/state error; failed calls do not release another mutation.
         */
        [[nodiscard]] Result<void> EndMutation(SaveBarrierMutation mutation, CanonicalCaptureEpoch epoch);
        /** @brief Publishes immutable version readiness or an explicit denial for an idle producer.
         * @param participant Registered index. @param epoch Non-zero coherent version (zero only for denial).
         * @param denial Typed inability to quiesce. @return Success or typed affinity/state error.
         */
        [[nodiscard]] Result<void> PublishReadiness(std::size_t participant, CanonicalCaptureEpoch epoch,
                                                    SaveBarrierDenial denial = SaveBarrierDenial::None);
        /** @brief Requests bounded quiescence without pausing simulation or invoking participant code.
         * @param operation Non-zero application identity. @param generation Exact runtime/scene/registry generation.
         * @return Success or typed invalid/busy/lifecycle error.
         */
        [[nodiscard]] Result<void> Request(OperationId operation, SaveRuntimeGeneration generation);
        /** @brief Checks all authorities and captures once at the current declared owner safe point.
         * @param phase Must be CommitDeferredLifecycleChanges. @param generation Exact current incarnation.
         * @param provenance Current coherent epoch/source revision, matching generation and registry.
         * @param participants Pinned actual registry snapshot. @param limits Existing canonical capture budgets.
         * @return Pending/deferred/failed/captured evidence or typed call validation error.
         * @post Every outcome resumes mutation admission; encoding and storage use only the detached snapshot.
         */
        [[nodiscard]] Result<SaveCaptureBarrierOutcome> CaptureAtSafePoint(RuntimePhase phase, SaveRuntimeGeneration generation,
                                                                           const RuntimeSaveCaptureProvenance &provenance,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           const RuntimeSaveCaptureLimits &limits = {});
        /** @brief Cancels one exact pending request without revoking live mutation tickets.
         * @param operation Exact current application identity. @return Success or typed invalid/reentrant/clock error.
         * @post A matched pending request becomes Cancelled even if its clock throws; elapsed retains
         * the last successful sample. Callers must acknowledge the terminal request after a timing error.
         */
        [[nodiscard]] Result<void> Cancel(OperationId operation);
        /** @brief Permanently closes request and mutation admission; outstanding mutations may finish.
         * @return Success or typed affinity/reentrant error. Idempotent.
         */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Releases terminal request evidence; readiness is retained, never inferred from old tickets.
         * @param operation Exact terminal identity. @return Success or typed state error.
         */
        [[nodiscard]] Result<void> Acknowledge(OperationId operation);
        /** @brief Copies owner-thread request evidence. @return Snapshot or typed affinity error. */
        [[nodiscard]] Result<SaveCaptureBarrierSnapshot> Snapshot() const;

    private:
        struct Participant final {
            SaveParticipantId identity;
            SaveBarrierDomain domain{};
            std::uint64_t mutation{};
            CanonicalCaptureEpoch epoch;
            SaveBarrierDenial denial{SaveBarrierDenial::None};
        };

        [[nodiscard]] Result<void> ValidateOwner() const;
        [[nodiscard]] Result<void> ValidateParticipant(std::size_t participant) const;
        [[nodiscard]] Result<void> ValidateCapture(RuntimePhase phase, SaveRuntimeGeneration generation,
                                                   const RuntimeSaveCaptureProvenance &provenance,
                                                   const SaveParticipantRegistrySnapshot &participants) const;
        [[nodiscard]] bool IsReady(CanonicalCaptureEpoch epoch);
        [[nodiscard]] Result<SaveCaptureBarrierOutcome> CaptureReady(const RuntimeSaveCaptureProvenance &provenance,
                                                                     SaveParticipantRegistrySnapshot participants,
                                                                     const RuntimeSaveCaptureLimits &limits);
        [[nodiscard]] Result<SaveCaptureBarrierOutcome> Outcome(std::optional<RuntimeSaveSnapshot> capture = {},
                                                                std::optional<Error> error = {}) const;
        void ApplyFailure(SaveBarrierReason reason);
        void MeasureElapsed();

        std::uint64_t authority_{};
        Clock *clock_{};
        std::thread::id owner_;
        std::size_t capacity_{};
        SaveCaptureBarrierPolicy policy_;
        std::vector<Participant> participants_;
        SaveCaptureBarrierSnapshot snapshot_;
        Duration requestedAt_;
        Duration lastSample_;
        std::uint64_t serial_{};
        bool capturing_{};
        bool closed_{};
    };
}  // namespace Horo::Runtime
