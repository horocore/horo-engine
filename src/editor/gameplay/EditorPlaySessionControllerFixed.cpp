#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/gameplay/EditorPlaySessionController.h"

#include <exception>

namespace Horo::Editor {
    namespace {
        Result<void> InvalidFixedAttempt() {
            return Result<void>::Failure(MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent,
                                                   "Play fixed callback does not match its actual scheduler attempt."));
        }

        bool MatchesContext(const Runtime::FixedStepContext &context, const Runtime::RuntimeDispatchFacts &facts) noexcept {
            return context.frameNumber == facts.frame && context.simulationTick == facts.fixedTick &&
                   context.attemptNumber == facts.fixedAttempt && context.fixedDelta == facts.fixedDuration;
        }
    }  // namespace

    Result<void> EditorPlaySessionController::BindRuntimeDispatch(const Runtime::RuntimeDispatchSource &source) {
        const auto status = source.BindingStatus();
        if (status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Reentrant)
            return InvalidFixedAttempt();
        if (dispatchSource_.IsValid() || state_ != EditorPlaySessionState::Idle)
            return InvalidFixedAttempt();
        dispatchSource_ = source;
        return Result<void>::Success();
    }

    Result<void> EditorPlaySessionController::OnRuntimeFixedUpdate(std::span<const Gameplay::GameplayInputAction> input,
                                                                   const Runtime::FixedStepContext &context) {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return InvalidFixedAttempt();
        if (!MatchesContext(context, facts) || fixedAttempt_ || facts.fixedTick == 0 || facts.committedTick >= facts.fixedTick)
            return InvalidFixedAttempt();
        FlushPendingStop();
        if (auto prepared = AdvancePreparation(); prepared.HasError())
            return prepared;
        if (!ShouldTick())
            return Result<void>::Success();
        fixedAttempt_.emplace(FixedAttempt{sceneHost_.get(), context.dispatchEvidence, facts, state_ == EditorPlaySessionState::Paused});
        const Gameplay::FixedDeltaTime delta{static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0};
        if (auto gameplay = RunGameplayFixed(input, delta); gameplay.HasError())
            return RejectFixedAttempt(context, gameplay.ErrorValue());
        if (auto native = fixedAttempt_->scene->OnRuntimeFixedUpdate(context); native.HasError())
            return RejectFixedAttempt(context, native.ErrorValue());
        return Result<void>::Success();
    }

    bool EditorPlaySessionController::MatchesFixedAttempt(const Runtime::FixedStepContext &context) const noexcept {
        return fixedAttempt_ && fixedAttempt_->evidence.RefersToSameDispatch(context.dispatchEvidence) &&
               MatchesContext(context, fixedAttempt_->facts);
    }

    Result<void> EditorPlaySessionController::RejectFixedAttempt(const Runtime::FixedStepContext &context, Error error) {
        AbortRuntimeFixedUpdate(context);
        Fail(error);
        return Result<void>::Failure(std::move(error));
    }

    Result<void> EditorPlaySessionController::PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context) {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return InvalidFixedAttempt();
        if (!fixedAttempt_)
            return Result<void>::Success();
        if (!MatchesFixedAttempt(context) || fixedAttempt_->prepared || facts.committedTick >= facts.fixedTick)
            return InvalidFixedAttempt();
        if (stopPending_) {
            AbortRuntimeFixedUpdate(context);
            FlushPendingStop();
            return Result<void>::Success();
        }
        if (auto prepared = fixedAttempt_->scene->PrepareRuntimeFixedCommit(context); prepared.HasError())
            return RejectFixedAttempt(context, prepared.ErrorValue());
        if (stopPending_) {
            AbortRuntimeFixedUpdate(context);
            FlushPendingStop();
            return Result<void>::Success();
        }
        fixedAttempt_->prepared = true;
        return Result<void>::Success();
    }

    void EditorPlaySessionController::CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            std::terminate();
        if (!fixedAttempt_)
            return;
        if (!MatchesFixedAttempt(context) || !fixedAttempt_->prepared || facts.committedTick != fixedAttempt_->facts.fixedTick ||
            facts.committedAttempt != fixedAttempt_->facts.fixedAttempt || facts.committedFrame != fixedAttempt_->facts.frame ||
            facts.committedDuration != fixedAttempt_->facts.fixedDuration)
            std::terminate();
        fixedAttempt_->scene->CommitRuntimeFixedUpdate(context);
        if (fixedAttempt_->consumesStep)
            stepPending_ = false;
        fixedAttempt_.reset();
    }

    void EditorPlaySessionController::AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        Runtime::RuntimeDispatchFacts ignored;
        const auto status = context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, ignored);
        if (status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Retired)
            return;
        if (!MatchesFixedAttempt(context))
            return;
        fixedAttempt_->scene->AbortRuntimeFixedUpdate(context);
        fixedAttempt_.reset();
    }

    void EditorPlaySessionController::FlushPendingStop() noexcept {
        if (stopPending_ && !fixedAttempt_)
            Stop();
    }
}  // namespace Horo::Editor
