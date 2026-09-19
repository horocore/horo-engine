#include "Horo/Runtime/FrameScheduler.h"

#include "../lifecycle/RuntimeErrors.h"
#include "Horo/Runtime/RuntimeLifecycle.h"

#include <memory>

namespace Horo::Runtime {
    namespace {
        constexpr Duration kZero{};

        [[nodiscard]] Result<void> CancelledResult() {
            return Result<void>::Failure(MakeError(RuntimeErrors::Cancelled));
        }

        [[nodiscard]] bool IsPositive(const Duration value) noexcept {
            return value > kZero;
        }
    }  // namespace

    /** @copydoc FrameClock::FrameClock */
    FrameClock::FrameClock(Clock &clock) noexcept : clock_(&clock) {}

    /** @copydoc FrameClock::Sample */
    Duration FrameClock::Sample() noexcept {
        const Duration now = clock_->MonotonicNow();
        if (!hasPrevious_) {
            previous_ = now;
            hasPrevious_ = true;
            return {};
        }
        const Duration elapsed = now - previous_;
        previous_ = now;
        return elapsed;
    }

    /** @copydoc FrameClock::Reset */
    void FrameClock::Reset() noexcept {
        hasPrevious_ = false;
        previous_ = {};
    }

    /** @copydoc FrameScheduler::Create */
    Result<std::unique_ptr<FrameScheduler>> FrameScheduler::Create(Clock &clock, FrameSchedulerConfig config) {
        if (!IsPositive(config.fixedStep) || !IsPositive(config.maximumFrameDelta) || config.maximumCatchUpSteps == 0) {
            return Result<std::unique_ptr<FrameScheduler>>::Failure(MakeError(RuntimeErrors::InvalidSchedulerConfig));
        }
        return Result<std::unique_ptr<FrameScheduler>>::Success(std::make_unique<FrameScheduler>(clock, config, ConstructionKey{}));
    }

    FrameScheduler::FrameScheduler(Clock &clock, const FrameSchedulerConfig config, ConstructionKey) noexcept
        : clock_(clock), config_(config) {}

    /** @copydoc FrameScheduler::NormalizeSampleDelta */
    void FrameScheduler::NormalizeSampleDelta(const Duration rawDelta, Duration &variableDelta, bool &clamped) {
        if (rawDelta < kZero) {
            variableDelta = {};
            ++statistics_.negativeDeltaNormalizationCount;
        } else if (rawDelta > config_.maximumFrameDelta) {
            variableDelta = config_.maximumFrameDelta;
            clamped = true;
            ++statistics_.maximumDeltaClampCount;
        }
    }

    /** @copydoc FrameScheduler::DispatchPhaseChecked */
    Result<void> FrameScheduler::DispatchPhaseChecked(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                      const FrameContext &context, const RuntimePhase phase) const {
        if (cancellation.IsCancellationRequested())
            return CancelledResult();
        if (Result<void> result = lifecycle.DispatchPhase(phase, context); result.HasError())
            return result;
        if (cancellation.IsCancellationRequested())
            return CancelledResult();
        return Result<void>::Success();
    }

    /** @copydoc FrameScheduler::DispatchPumpPhases */
    Result<void> FrameScheduler::DispatchPumpPhases(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                    const FrameContext &context, const bool suspended) {
        using enum RuntimePhase;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, BeginFrame); result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, PollPlatformEvents); result.HasError())
            return result;
        if (!suspended) {
            if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, BuildInputSnapshot); result.HasError())
                return result;
        }
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, ApplyQueuedOwnerThreadCommands); result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, NetworkPoll); result.HasError())
            return result;

        if (suspended) {
            if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, context, EndFrame); result.HasError())
                return result;
            clock_.Reset();
            return Result<void>::Success();
        }
        return Result<void>::Success();
    }

    /** @copydoc FrameScheduler::RunFixedSteps */
    Result<void> FrameScheduler::RunFixedSteps(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                               Duration &droppedSimulationTime) {
        droppedSimulationTime = {};
        std::uint32_t executedSteps = 0;
        while (accumulator_ >= config_.fixedStep && executedSteps < config_.maximumCatchUpSteps) {
            if (cancellation.IsCancellationRequested())
                return CancelledResult();
            const FixedStepContext fixedContext{.simulationTick = completedSimulationTick_ + 1,
                                                .fixedDelta = config_.fixedStep,
                                                .cancellation = cancellation};
            if (Result<void> result = lifecycle.DispatchFixedUpdate(fixedContext); result.HasError())
                return result;
            if (cancellation.IsCancellationRequested())
                return CancelledResult();
            accumulator_ -= config_.fixedStep;
            ++completedSimulationTick_;
            statistics_.completedSimulationTick = completedSimulationTick_;
            ++executedSteps;
        }

        if (accumulator_ >= config_.fixedStep) {
            const auto droppedSteps = static_cast<std::uint64_t>(accumulator_.ToNanoseconds() / config_.fixedStep.ToNanoseconds());
            const auto dropped = Duration::FromNanoseconds(static_cast<std::int64_t>(droppedSteps) * config_.fixedStep.ToNanoseconds());
            accumulator_ -= dropped;
            statistics_.totalDroppedSimulationTime += dropped;
            statistics_.totalDroppedFixedSteps += droppedSteps;
            ++statistics_.catchUpLimitedFrameCount;
            droppedSimulationTime = dropped;
        }
        return Result<void>::Success();
    }

    /** @copydoc FrameScheduler::RunFrame */
    Result<void> FrameScheduler::RunFrame(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation, const bool suspended) {
        ++frameNumber_;
        const Duration rawDelta = suspended ? Duration{} : clock_.Sample();
        Duration variableDelta = rawDelta;
        bool clamped = false;
        NormalizeSampleDelta(rawDelta, variableDelta, clamped);

        FrameContext frameContext{.frameNumber = frameNumber_,
                                  .variableDelta = variableDelta,
                                  .interpolationAlpha = 0.0,
                                  .completedSimulationTick = completedSimulationTick_,
                                  .droppedSimulationTime = {},
                                  .realDeltaWasClamped = clamped,
                                  .cancellation = cancellation};

        if (Result<void> result = DispatchPumpPhases(lifecycle, cancellation, frameContext, suspended); result.HasError())
            return result;
        if (suspended)
            return Result<void>::Success();

        accumulator_ += variableDelta;
        Duration droppedSimulationTime{};
        if (Result<void> result = RunFixedSteps(lifecycle, cancellation, droppedSimulationTime); result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::NetworkFlush);
            result.HasError())
            return result;
        frameContext.droppedSimulationTime = droppedSimulationTime;
        frameContext.completedSimulationTick = completedSimulationTick_;
        frameContext.interpolationAlpha =
            static_cast<double>(accumulator_.ToNanoseconds()) / static_cast<double>(config_.fixedStep.ToNanoseconds());

        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::VariableUpdate);
            result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::RenderExtraction);
            result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::RenderExecution);
            result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::RenderGui); result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::Presentation);
            result.HasError())
            return result;
        if (Result<void> result = DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::CommitDeferredLifecycleChanges);
            result.HasError())
            return result;
        return DispatchPhaseChecked(lifecycle, cancellation, frameContext, RuntimePhase::EndFrame);
    }

    /** @copydoc FrameScheduler::ResetClock */
    void FrameScheduler::ResetClock() noexcept {
        clock_.Reset();
    }

    /** @copydoc FrameScheduler::Statistics */
    FrameSchedulerStatistics FrameScheduler::Statistics() const noexcept {
        return statistics_;
    }
}  // namespace Horo::Runtime
