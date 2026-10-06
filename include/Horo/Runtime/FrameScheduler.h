#pragma once

/**
 * @file FrameScheduler.h
 * @brief Fixed-step timing and canonical runtime frame phase contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"

#include <memory>

namespace Horo::Runtime {
    class RuntimeLifecycle;

    /** @brief Canonical owner-thread phases shared by graphical and headless hosts. */
    enum class RuntimePhase : std::uint8_t {
        BeginFrame,
        PollPlatformEvents,
        BuildInputSnapshot,
        ApplyQueuedOwnerThreadCommands,
        NetworkPoll,
        FixedUpdate, /**< Ordering marker delivered through OnFixedUpdate rather than OnPhase. */
        NetworkFlush,
        VariableUpdate,
        RenderExtraction,
        RenderExecution,
        RenderGui,
        Presentation,
        CommitDeferredLifecycleChanges,
        EndFrame,
    };

    /** @brief Presentation baseline continuity supplied exclusively by the host scheduler. */
    enum class PresentationClockContinuity : std::uint8_t {
        Initial,
        Continuous,
        BaselineReset,
    };

    /** @brief Validated fixed-step and stall-normalization policy. */
    struct FrameSchedulerConfig {
        Duration fixedStep{Duration::FromNanoseconds(16'666'667)};
        /**< Simulation duration committed by one fixed tick. */
        Duration maximumFrameDelta{Duration::FromMilliseconds(250)};
        /**< Largest real delta admitted to the accumulator. */
        std::uint32_t maximumCatchUpSteps{5};  /**< Maximum fixed ticks executed in one frame. */
        std::uint32_t maximumPauseLeases{64};  /**< Preallocated composed host pause requests. */
        std::uint32_t maximumStepReceipts{64}; /**< Preallocated pending and retained terminal step requests. */
    };

    /** @brief Allocation-free cumulative timing diagnostics owned by one scheduler. */
    struct FrameSchedulerStatistics {
        std::uint64_t completedSimulationTick{};         /**< Count of fixed ticks committed successfully. */
        Duration totalDroppedSimulationTime{};           /**< Whole fixed-step time discarded after catch-up saturation. */
        std::uint64_t totalDroppedFixedSteps{};          /**< Number of discarded fixed-step intervals. */
        std::uint64_t catchUpLimitedFrameCount{};        /**< Frames that discarded accumulated simulation time. */
        std::uint64_t negativeDeltaNormalizationCount{}; /**< Backward clock samples normalized to zero. */
        std::uint64_t maximumDeltaClampCount{};          /**< Samples clamped to maximumFrameDelta. */
    };

    /**
     * @brief Exact last fixed dispatch committed by the scheduler after every participant and cancellation check succeeds.
     * @details Tick zero denotes no commitment. Attempts are monotonic across failed/retried ticks; duration/frame describe
     *          this successful dispatch, never the failed attempt that preceded it. Consumers must match their staged attempts
     *          and consume evidence only when their own complete publication succeeds.
     */
    struct CommittedFixedStepEvidence final {
        std::uint64_t simulationTick{};
        std::uint64_t attemptNumber{};
        std::uint64_t frameNumber{};
        Duration duration{};
    };

    /** @brief Immutable context visible to one variable-rate runtime phase. */
    struct FrameContext {
        std::uint64_t frameNumber{};                  /**< One-based host frame ordinal. */
        Duration variableDelta{};                     /**< Real delta after negative and maximum normalization. */
        double interpolationAlpha{};                  /**< Fractional accumulator position in the range [0, 1). */
        std::uint64_t completedSimulationTick{};      /**< Count of fixed ticks committed before this phase. */
        Duration droppedSimulationTime{};             /**< Whole fixed-step time discarded during this frame. */
        bool realDeltaWasClamped{false};              /**< Whether maximumFrameDelta changed the real sample. */
        const CancellationToken &cancellation;        /**< Host-owned cooperative cancellation token. */
        std::uint64_t presentationClockGeneration{1}; /**< Scheduler-owned baseline identity; independent from simulation ticks. */
        PresentationClockContinuity presentationContinuity{PresentationClockContinuity::Initial};
        /**< Explicit baseline admission evidence, not inferred from a zero delta. */
        CommittedFixedStepEvidence committedFixedStep; /**< Exact successful fixed dispatch fence, copied by the scheduler. */
        Duration presentationAdmittedDuration{};
        /**< Cumulative normalized presentation duration admitted before dispatch. Never reset by suspension/baseline changes;
         * consumers subtract only their successfully published cursor, preserving unread time across failed UI candidates. */
        RuntimeSimulationPolicy simulationPolicy; /**< Actual last owner cutoff facts; grants no mutable timing authority. */
        RuntimeDispatchEvidence dispatchEvidence; /**< Default-invalid unless privately issued for this actual active dispatch. */
    };

    /** @brief Immutable context for the fixed tick currently being attempted. */
    struct FixedStepContext {
        std::uint64_t simulationTick{};           /**< One-based tick ordinal; committed only after every participant succeeds. */
        Duration fixedDelta{};                    /**< Validated fixed simulation duration. */
        const CancellationToken &cancellation;    /**< Host-owned cooperative cancellation token. */
        std::uint64_t attemptNumber{};            /**< Scheduler-issued ordinal; a same-tick retry receives a new attempt. */
        std::uint64_t frameNumber{};              /**< Actual host frame in which this fixed dispatch is attempted. */
        RuntimeDispatchEvidence dispatchEvidence; /**< Admission only; never proof that the fixed dispatch succeeded. */
    };

    /** @brief Samples an injected monotonic clock and owns resume-safe frame baselines. */
    class FrameClock final {
    public:
        /** @brief Binds a host-owned clock. @param clock Clock that outlives this object. */
        explicit FrameClock(Clock &clock) noexcept;

        /** @brief Samples elapsed time; the first sample after Reset returns zero. @return Signed raw elapsed duration. */
        [[nodiscard]] Duration Sample() noexcept;

        /** @brief Discards the current baseline so suspended wall time cannot enter the next frame. */
        void Reset() noexcept;

    private:
        Clock *clock_{};
        Duration previous_{};
        bool hasPrevious_{false};
    };

    /** @brief Serial owner-thread scheduler for canonical phases and fixed simulation ticks. */
    class FrameScheduler final {
    public:
        /**
         * @brief Creates a scheduler after validating its timing policy.
         * @param clock Host-owned monotonic clock.
         * @param config Fixed-step and stall policy.
         * @return Owned scheduler or a typed invalid-configuration error.
         */
        [[nodiscard]] static Result<std::unique_ptr<FrameScheduler>> Create(Clock &clock, FrameSchedulerConfig config = {});

        /**
         * @brief Executes one running or suspended frame through the supplied lifecycle.
         * @param lifecycle Started lifecycle whose participants receive callbacks.
         * @param cancellation Host cancellation token.
         * @param suspended Whether only the suspend-safe pump subset should execute.
         * @return Success, the original participant failure, cancellation, or a typed scheduler failure.
         */
        [[nodiscard]] Result<void> RunFrame(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation, bool suspended);

        /**
         * @brief Resets the presentation baseline and advances its never-wrapping generation.
         * @details Simulation commitment and accumulator are unchanged. Generation exhaustion is latched and the next
         *          RunFrame returns a typed failure before dispatch; the void/noexcept reset contract is preserved.
         */
        void ResetClock() noexcept;

        /** @brief Revokes source admission before explicit host teardown. Retained read pins remain safe. */
        void RetireDispatch() noexcept;

        /** @brief Copies the actual read-only producer issuer for explicit application composition. @return Retained issuer capability. */
        [[nodiscard]] RuntimeDispatchSource DispatchSource() const noexcept {
            return dispatchSource_;
        }

        /** @brief Retires producer admission before final scheduler destruction. */
        ~FrameScheduler();

        /** @brief Returns a lock-free value snapshot of cumulative scheduler diagnostics. */
        [[nodiscard]] FrameSchedulerStatistics Statistics() const noexcept;

        /** @brief Returns actual owner-thread timing command capability. @return Scheduler-owned control; never retain beyond scheduler. */
        [[nodiscard]] RuntimeSimulationControl &SimulationControl() noexcept {
            return simulationControl_;
        }

        /** @brief Borrows actual read-only timing control. @return Const scheduler-owned capability. */
        [[nodiscard]] const RuntimeSimulationControl &SimulationControl() const noexcept {
            return simulationControl_;
        }

    private:
        friend class RuntimeHost;

        /**
         * @brief Internal construction key so only Create can build a scheduler.
         * @details Private default constructor with FrameScheduler as friend:
         *          make_unique inside Create works, external callers cannot
         *          construct a key and thus cannot bypass Create.
         */
        class ConstructionKey {
            ConstructionKey() = default;
            friend class FrameScheduler;
        };

    public:
        FrameScheduler(Clock &clock, FrameSchedulerConfig config, ConstructionKey) noexcept;

    private:
        /** @brief Normalizes one raw clock sample into the accumulator domain. */

        void NormalizeSampleDelta(Duration rawDelta, Duration &variableDelta, bool &clamped);

        /** @brief Dispatches one phase with cooperative cancellation guards. */
        [[nodiscard]] Result<void> DispatchPhaseChecked(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                        FrameContext &context, RuntimePhase phase);

        /** @brief Runs the suspend-safe pump phases; returns Success after EndFrame when suspended. */
        [[nodiscard]] Result<void> DispatchPumpPhases(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                      FrameContext &context, bool suspended);

        /** @brief Executes fixed ticks within catch-up limits and drops saturated simulation time. */
        [[nodiscard]] Result<void> RunFixedSteps(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                 Duration &droppedSimulationTime);

        /** @brief Dispatch reservation revoked exactly once on every callback exit. */
        struct DispatchGuard final {
            explicit DispatchGuard(RuntimeDispatchSource &source) noexcept : source(source) {}

            ~DispatchGuard() {
                source.End();
            }

            DispatchGuard(const DispatchGuard &) = delete;
            DispatchGuard &operator=(const DispatchGuard &) = delete;
            DispatchGuard(DispatchGuard &&) = delete;
            DispatchGuard &operator=(DispatchGuard &&) = delete;
            RuntimeDispatchSource &source;
        };

        /** @brief Checks dispatch ownership before any new frame or accumulator mutation. */
        [[nodiscard]] Result<void> AdmitRun() const;
        /** @brief Copies trusted producer values for one actual variable-rate phase. */
        [[nodiscard]] RuntimeDispatchFacts FrameFacts(const FrameContext &context) const noexcept;
        /** @brief Admits one actual fixed dispatch, revokes its proof, and retains the original callback/cancellation failure. */
        [[nodiscard]] Result<void> DispatchFixedChecked(RuntimeLifecycle &lifecycle, FixedStepContext &context);

        RuntimeDispatchSource dispatchSource_;
        std::uint64_t completedVariableUpdateFrame_{};
        RuntimeSimulationControl simulationControl_;
        FrameClock clock_;
        FrameSchedulerConfig config_;
        FrameSchedulerStatistics statistics_;
        Duration accumulator_{};
        std::uint64_t frameNumber_{};
        std::uint64_t completedSimulationTick_{};
        std::uint64_t fixedAttemptNumber_{};
        CommittedFixedStepEvidence committedFixedStep_;
        std::uint64_t presentationClockGeneration_{1};
        PresentationClockContinuity presentationContinuity_{PresentationClockContinuity::Initial};
        bool presentationClockExhausted_{};
        Duration presentationAdmittedDuration_{};
        bool presentationDurationExhausted_{};
    };
}  // namespace Horo::Runtime
