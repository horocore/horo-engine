#pragma once

/**
 * @file FrameScheduler.h
 * @brief Fixed-step timing and canonical runtime frame phase contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Result.h"

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

    /** @brief Validated fixed-step and stall-normalization policy. */
    struct FrameSchedulerConfig {
        Duration fixedStep{Duration::FromNanoseconds(16'666'667)};
        /**< Simulation duration committed by one fixed tick. */
        Duration maximumFrameDelta{Duration::FromMilliseconds(250)};
        /**< Largest real delta admitted to the accumulator. */
        std::uint32_t maximumCatchUpSteps{5}; /**< Maximum fixed ticks executed in one frame. */
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

    /** @brief Immutable context visible to one variable-rate runtime phase. */
    struct FrameContext {
        std::uint64_t frameNumber{};             /**< One-based host frame ordinal. */
        Duration variableDelta{};                /**< Real delta after negative and maximum normalization. */
        double interpolationAlpha{};             /**< Fractional accumulator position in the range [0, 1). */
        std::uint64_t completedSimulationTick{}; /**< Count of fixed ticks committed before this phase. */
        Duration droppedSimulationTime{};        /**< Whole fixed-step time discarded during this frame. */
        bool realDeltaWasClamped{false};         /**< Whether maximumFrameDelta changed the real sample. */
        const CancellationToken &cancellation;   /**< Host-owned cooperative cancellation token. */
    };

    /** @brief Immutable context for the fixed tick currently being attempted. */
    struct FixedStepContext {
        std::uint64_t simulationTick{};        /**< One-based tick ordinal; committed only after every participant succeeds. */
        Duration fixedDelta{};                 /**< Validated fixed simulation duration. */
        const CancellationToken &cancellation; /**< Host-owned cooperative cancellation token. */
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

        /** @brief Resets the real-time baseline before resuming from suspension. */
        void ResetClock() noexcept;

        /** @brief Returns a lock-free value snapshot of cumulative scheduler diagnostics. */
        [[nodiscard]] FrameSchedulerStatistics Statistics() const noexcept;

    private:
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
                                                        const FrameContext &context, RuntimePhase phase) const;

        /** @brief Runs the suspend-safe pump phases; returns Success after EndFrame when suspended. */
        [[nodiscard]] Result<void> DispatchPumpPhases(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                      const FrameContext &context, bool suspended);

        /** @brief Executes fixed ticks within catch-up limits and drops saturated simulation time. */
        [[nodiscard]] Result<void> RunFixedSteps(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                 Duration &droppedSimulationTime);

        FrameClock clock_;
        FrameSchedulerConfig config_;
        FrameSchedulerStatistics statistics_;
        Duration accumulator_{};
        std::uint64_t frameNumber_{};
        std::uint64_t completedSimulationTick_{};
    };
}  // namespace Horo::Runtime
