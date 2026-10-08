#include "../lifecycle/RuntimeErrors.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/RuntimeLifecycle.h"

namespace Horo::Runtime {
    namespace {
        /** @brief Translates exact producer admission failure at the scheduler boundary. */
        [[nodiscard]] Result<void> DispatchAdmissionResult(const RuntimeDispatchStatus status) {
            using enum RuntimeDispatchStatus;
            if (status == Valid)
                return Result<void>::Success();
            const auto *error = &RuntimeErrors::DispatchInvalid;
            if (status == Reentrant)
                error = &RuntimeErrors::DispatchReentrant;
            else if (status == Exhausted)
                error = &RuntimeErrors::DispatchExhausted;
            else if (status == Retired)
                error = &RuntimeErrors::DispatchRetired;
            return Result<void>::Failure(MakeError(*error));
        }
    }  // namespace

    /** @copydoc FrameScheduler::~FrameScheduler */
    FrameScheduler::~FrameScheduler() {
        RetireDispatch();
    }

    /** @copydoc FrameScheduler::RetireDispatch */
    void FrameScheduler::RetireDispatch() noexcept {
        dispatchSource_.Retire();
    }

    /** @copydoc FrameScheduler::AdmitRun */
    Result<void> FrameScheduler::AdmitRun() const {
        return DispatchAdmissionResult(dispatchSource_.ValidateRun());
    }

    /** @copydoc FrameScheduler::FrameFacts */
    RuntimeDispatchFacts FrameScheduler::FrameFacts(const FrameContext &context) const noexcept {
        return {.frame = context.frameNumber,
                .committedTick = context.committedFixedStep.simulationTick,
                .committedAttempt = context.committedFixedStep.attemptNumber,
                .committedFrame = context.committedFixedStep.frameNumber,
                .committedDuration = context.committedFixedStep.duration,
                .presentationAdmittedDuration = context.presentationAdmittedDuration,
                .presentationGeneration = context.presentationClockGeneration,
                .presentationReset = context.presentationContinuity == PresentationClockContinuity::BaselineReset,
                .presentationClamped = context.realDeltaWasClamped,
                .completedVariableUpdateFrame = completedVariableUpdateFrame_,
                .simulationPolicy = context.simulationPolicy};
    }

    /** @copydoc FrameScheduler::DispatchPhaseChecked */
    Result<void> FrameScheduler::DispatchPhaseChecked(RuntimeLifecycle &lifecycle, const CancellationToken &cancellation,
                                                      FrameContext &context, const RuntimePhase phase) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(RuntimeErrors::Cancelled));
        if (auto admitted = DispatchAdmissionResult(dispatchSource_.Begin(phase, FrameFacts(context))); admitted.HasError())
            return admitted;
        const DispatchGuard guard{dispatchSource_};
        context.dispatchEvidence = dispatchSource_.Evidence();
        if (Result<void> result = lifecycle.DispatchPhase(phase, context); result.HasError())
            return result;
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(RuntimeErrors::Cancelled));
        return Result<void>::Success();
    }

    /** @copydoc FrameScheduler::DispatchFixedChecked */
    Result<void> FrameScheduler::DispatchFixedChecked(RuntimeLifecycle &lifecycle, FixedStepContext &context) {
        const RuntimeDispatchFacts facts{.frame = context.frameNumber,
                                         .fixedTick = context.simulationTick,
                                         .fixedAttempt = context.attemptNumber,
                                         .fixedDuration = context.fixedDelta,
                                         .committedTick = committedFixedStep_.simulationTick,
                                         .committedAttempt = committedFixedStep_.attemptNumber,
                                         .committedFrame = committedFixedStep_.frameNumber,
                                         .committedDuration = committedFixedStep_.duration,
                                         .simulationPolicy = simulationControl_.Policy()};
        if (auto admitted = DispatchAdmissionResult(dispatchSource_.Begin(RuntimePhase::FixedUpdate, facts)); admitted.HasError())
            return admitted;
        const DispatchGuard guard{dispatchSource_};
        context.dispatchEvidence = dispatchSource_.Evidence();
        if (auto result = lifecycle.DispatchFixedUpdate(context); result.HasError())
            return result;
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(RuntimeErrors::Cancelled));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
