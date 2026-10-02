#pragma once

/** @file SaveAutosaveScheduler.h
 * @brief Bounded host-owned autosave cadence and arbiter/barrier capture admission.
 */

#include "Horo/Runtime/Save/SaveCaptureBarrier.h"
#include "Horo/Runtime/Save/SaveOperationArbiter.h"

namespace Horo::Runtime {
    /** @brief Host-approved absolute clock selected for autosave cadence. */
    enum class SaveAutosaveTimeDomain : std::uint8_t {
        Gameplay,
        MonotonicRealTime
    };
    /** @brief Runtime activity at the supplied clock boundary. Only Active permits capture. */
    enum class SaveAutosaveActivity : std::uint8_t {
        Active,
        Paused,
        Loading,
        Inactive
    };
    /** @brief Whether a non-active interval contributes to cadence, cooldown and pending age. */
    enum class SaveAutosaveClockPolicy : std::uint8_t {
        Freeze,
        Accumulate
    };
    /** @brief Last scheduler disposition; pending intent is independently visible during admitted work. */
    enum class SaveAutosaveDisposition : std::uint8_t {
        Armed,
        Pending,
        Capturing,
        Saving,
        Completed,
        Failed,
        Cancelled,
        Closed
    };

    /** @brief Exact finite policy; zero jitter disables phase spreading and zero cooldown permits immediate admission.
     * Jitter is one deterministic session phase offset: seed modulo (jitter nanoseconds + 1).
     * Subsequent deadlines remain exactly interval apart, independent of polling and admission delays.
     */
    struct SaveAutosavePolicy final {
        Duration interval{Duration::FromMilliseconds(300'000)};
        Duration cooldown{Duration::FromMilliseconds(30'000)};
        Duration jitter;
        std::uint64_t jitterSeed{};
        SaveAutosaveTimeDomain domain{SaveAutosaveTimeDomain::Gameplay};
        SaveAutosaveClockPolicy paused{SaveAutosaveClockPolicy::Freeze};
        SaveAutosaveClockPolicy loading{SaveAutosaveClockPolicy::Freeze};
        SaveAutosaveClockPolicy inactive{SaveAutosaveClockPolicy::Freeze};
    };

    /** @brief Nonnegative nanosecond clocks and exact session identity supplied by the host.
     * Gameplay time is cumulative committed simulation time, not frame delta or wall time.
     * Samples close the interval under the previous activity, then install the new activity.
     */
    struct SaveAutosaveClockSample final {
        SaveRuntimeGeneration generation;
        Duration gameplay;
        Duration monotonic;
        SaveAutosaveActivity activity{SaveAutosaveActivity::Active};
    };

    /** @brief Constant-size owner-thread diagnostics; counts and age saturate instead of wrapping. */
    struct SaveAutosaveSchedulerSnapshot final {
        SaveRuntimeGeneration generation;
        SaveAutosaveDisposition disposition{SaveAutosaveDisposition::Armed};
        bool pending{}; /**< One latest-state intent, never a retained runtime snapshot. */
        bool blocked{}; /**< A failed/deferred capture or operation requires explicit host recovery. */
        Duration pendingAge;
        Duration untilNextTrigger;
        Duration cooldownRemaining;
        std::uint64_t triggers{};
        std::uint64_t coalescedTriggers{};
        OperationId operation{};
        std::optional<SaveCaptureBarrierSnapshot> lastBarrier; /**< Exact denial, timing and epoch of the most recent capture poll. */
    };

    /** @brief Detached immutable capture and existing operation lease transferred to host worker composition. */
    struct SaveAutosaveCapture final {
        SaveRuntimeGeneration generation;
        SaveOperationHandle operation;
        RuntimeSaveSnapshot snapshot;
    };

    /**
     * @brief Session-owned scheduler borrowing the sole arbiter and capture barrier on their owner thread.
     *
     * Sample is allocation-free on valid inputs and does no I/O. Missed periods are reduced in O(1)
     * to one visible intent. CommitAtSafePoint performs bounded admission/capture only. The host owns
     * encoding/storage, arbiter terminal publication/acknowledgement, retry policy and ring rotation.
     * Dependencies outlive this object; destroy/close on the owner after capture callbacks return.
     */
    class SaveAutosaveScheduler final {
    public:
        /** @brief Validates policy/clocks and creates one owner-thread scheduler.
         * @param policy Positive interval, nonnegative cooldown/jitter with representable interval+jitter.
         * @param initial Nonnegative baseline and valid generation/activity.
         * @param arbiter Existing session operation authority, borrowed for this object's lifetime.
         * @param barrier Existing session capture authority, borrowed for this object's lifetime.
         * @return Scheduler or typed policy/allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<SaveAutosaveScheduler>> Create(const SaveAutosavePolicy &policy,
                                                                                   const SaveAutosaveClockSample &initial,
                                                                                   SaveOperationArbiter &arbiter,
                                                                                   SaveCaptureBarrier &barrier);
        /** @brief Closes on the owner without waiting; detached producers remain host-owned. */
        ~SaveAutosaveScheduler();
        SaveAutosaveScheduler(const SaveAutosaveScheduler &) = delete;
        SaveAutosaveScheduler &operator=(const SaveAutosaveScheduler &) = delete;

    private:
        /** @brief Factory-issued construction capability; external callers cannot create a key. */
        class ConstructionKey {
            ConstructionKey() = default;
            friend class SaveAutosaveScheduler;
        };

    public:
        /** @brief Factory-only construction after policy, clock and authority validation.
         * @param policy Validated timing policy. @param initial Validated clock baseline.
         * @param arbiter Borrowed operation authority. @param barrier Borrowed capture authority.
         * @param key Private construction capability issued only by Create.
         */
        SaveAutosaveScheduler(const SaveAutosavePolicy &policy, const SaveAutosaveClockSample &initial, SaveOperationArbiter &arbiter,
                              SaveCaptureBarrier &barrier, ConstructionKey key) noexcept;

        /** @brief Advances exact cadence, retaining one latest-state intent; no catch-up loop or allocation.
         * @param sample New absolute clocks; both must be nondecreasing even when frozen/unselected.
         * @return Success or typed stale/invalid/affinity/lifecycle error. Rejected samples change nothing.
         */
        [[nodiscard]] Result<void> Sample(const SaveAutosaveClockSample &sample);
        /** @brief Admits only when Active, cooldown-expired, unblocked and the arbiter and barrier are idle.
         * Manual/queued work takes precedence. Polls an owned pending barrier at subsequent safe points.
         * @param phase Current actual lifecycle phase, never a remembered previous safe point.
         * @param generation Exact current generation.
         * @param operation Fresh host OperationStore descriptor; used only for a new admission.
         * @param address Current typed autosave target chosen by host catalog/rotation policy.
         * @param provenance Current coherent capture evidence, never timer-time state.
         * @param participants Pinned actual registry snapshot.
         * @param limits Existing immutable capture limits.
         * @return Empty while deferred/busy; one handoff after capture; original typed admission/capture/operation failure.
         * @post No callback or runtime snapshot is retained after handoff. Reentry is rejected.
         */
        [[nodiscard]] Result<std::optional<SaveAutosaveCapture>> CommitAtSafePoint(RuntimePhase phase, SaveRuntimeGeneration generation,
                                                                                   SaveOperationDescriptor operation,
                                                                                   SaveArbiterAddress address,
                                                                                   const RuntimeSaveCaptureProvenance &provenance,
                                                                                   SaveParticipantRegistrySnapshot participants,
                                                                                   const RuntimeSaveCaptureLimits &limits = {});
        /** @brief Explicitly cancels pending intent and cooperatively cancels owned pre-commit work.
         * @return Success or typed affinity/reentrancy failure; post-commit completion remains host-owned.
         */
        [[nodiscard]] Result<void> Cancel();
        /** @brief Clears a visible failure block after host policy remedies the cause; preserves pending intent.
         * @return Success or typed lifecycle/affinity error. Does not implement storage retries.
         */
        [[nodiscard]] Result<void> Resume();
        /** @brief Cancels old intent, fences old completion and resets cadence for a new session incarnation.
         * @param initial New distinct generation and clock baseline. @return Success or typed validation error.
         */
        [[nodiscard]] Result<void> ReplaceSession(const SaveAutosaveClockSample &initial);
        /** @brief Closes scheduling and cancels owned pre-commit work without waiting or closing shared authorities.
         * @return Success or typed affinity/reentrancy error. Idempotent.
         */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Copies scalar diagnostics on the owner thread. @return Snapshot or affinity error. */
        [[nodiscard]] Result<SaveAutosaveSchedulerSnapshot> Snapshot() const;

    private:
        /** @brief Checks owner-thread affinity. */
        [[nodiscard]] Result<void> ValidateOwner() const;
        /** @brief Checks affinity, reentry and closed-admission invariants. */
        [[nodiscard]] Result<void> ValidateMutation() const;
        /** @brief Resets cadence and observation against a validated baseline. */
        void Reset(const SaveAutosaveClockSample &initial) noexcept;
        /** @brief Reduces nonnegative eligible elapsed time without looping or wrapping. */
        void AdvanceTime(std::int64_t delta) noexcept;
        /** @brief Cancels only this scheduler's producer and barrier request without waiting. */
        [[nodiscard]] Result<void> CancelOwned();
        /** @brief Releases the owned uncaptured barrier after cancellation, preserving errors for explicit cleanup. */
        [[nodiscard]] Result<void> CancelBarrier(SaveCancellationRequestResult cancelled);
        /** @brief Retires uncaptured terminal work and preserves its exact failure cause. */
        [[nodiscard]] Result<void> RetireCapture();
        /** @brief Checks bounded admission capacity without mutating either shared authority. */
        [[nodiscard]] Result<bool> ReadyForAdmission() const;
        /** @brief Acknowledges one barrier outcome and transfers only a successful immutable cut. */
        [[nodiscard]] Result<std::optional<SaveAutosaveCapture>> CompleteCapture(SaveCaptureBarrierOutcome outcome);
        /** @brief Observes the exact retained terminal handle and preserves failure cause. */
        [[nodiscard]] Result<void> ObserveTerminal();
        /** @brief Creates one background arbiter operation and requests its barrier. */
        [[nodiscard]] Result<void> Admit(SaveOperationDescriptor operation, SaveArbiterAddress address);
        /** @brief Polls cancellation and hands off the current immutable barrier cut once. */
        [[nodiscard]] Result<std::optional<SaveAutosaveCapture>> Capture(RuntimePhase phase, const RuntimeSaveCaptureProvenance &provenance,
                                                                         SaveParticipantRegistrySnapshot participants,
                                                                         const RuntimeSaveCaptureLimits &limits);

        SaveAutosavePolicy policy_;
        SaveAutosaveClockSample last_;
        SaveAutosaveSchedulerSnapshot snapshot_;
        SaveOperationArbiter *arbiter_;
        SaveCaptureBarrier *barrier_;
        std::thread::id owner_;
        SaveOperationHandle operation_;
        bool awaitingCapture_{};
        bool executing_{};
        bool closed_{};
    };
}  // namespace Horo::Runtime
