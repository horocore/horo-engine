#include "AnimationGraphSceneCommitAccess.h"
#include "SceneAnimationState.h"

#include <exception>

namespace Horo::Runtime {
    namespace A = Animation;

    bool SceneAnimationRuntime::State::ReadAttempt(const FixedStepContext &context, RuntimeDispatchFacts &facts) const noexcept {
        if (!OnOwner())
            return false;
        if (context.dispatchEvidence.Read(config.expectedDispatch, RuntimePhase::FixedUpdate, facts) != RuntimeDispatchStatus::Valid)
            return false;
        return facts.fixedTick == context.simulationTick && facts.fixedAttempt == context.attemptNumber &&
               facts.frame == context.frameNumber && facts.fixedDuration == context.fixedDelta;
    }

    Result<void> SceneAnimationRuntime::State::BeginAttempt(const FixedStepContext &context) {
        RuntimeDispatchFacts facts;
        if (!ReadAttempt(context, facts) || facts.fixedTick == 0 || facts.fixedDuration.ToNanoseconds() <= 0)
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        const auto admission = Admit(true);
        if (admission.HasError())
            return admission;
        if (!started)
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Unavailable));
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        attemptTick = A::AnimationTickId::Create(facts.fixedTick).Value();
        attemptEvidence = context.dispatchEvidence;
        attempting = true;
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::State::EvaluateActor(Actor &actor, const FixedStepContext &context) {
        if (!actor.playing)
            return Result<void>::Success();
        const auto snapshot = config.inputs ? config.inputs->Read(actor.entity, context) : SceneAnimationInputSnapshot{};
        const A::GraphEvaluationInput input{attemptTick,           {context.fixedDelta.ToNanoseconds()},
                                            snapshot.playbackRate, snapshot.parameters,
                                            context.cancellation,  snapshot.curveTime};
        const auto outcome = actor.graph.Evaluate(input);
        if (outcome.HasError())
            return Result<void>::Failure(A::DescribeGraphInstanceFailure(outcome));
        actor.staged = true;
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::State::PrepareAttempt(const FixedStepContext &context) {
        RuntimeDispatchFacts facts;
        if (!ReadAttempt(context, facts))
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        const auto admission = Admit();
        if (admission.HasError())
            return admission;
        if (!attempting || armed || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        auto prepared = ArmActors();
        if (prepared.HasError())
            return prepared;
        armed = true;
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::State::ArmActors() {
        if (!active || active->closed)
            return Result<void>::Success();
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            const auto outcome = A::Detail::GraphSceneCommitAccess::Prepare(actor->graph, attemptTick);
            if (outcome.HasError())
                return Result<void>::Failure(A::DescribeGraphInstanceFailure(outcome));
        }
        return Result<void>::Success();
    }

    void SceneAnimationRuntime::State::PublishActors() noexcept {
        if (!active || active->closed)
            return;
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            (void)A::Detail::GraphSceneCommitAccess::Publish(actor->graph);
            actor->staged = false;
        }
    }

    void SceneAnimationRuntime::State::AbortActors() noexcept {
        if (!active || active->closed)
            return;
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            if (actor->graph.Abort().HasError())
                std::terminate();
            actor->staged = false;
        }
    }

    void SceneAnimationRuntime::State::ClearAttempt() noexcept {
        armed = false;
        attempting = false;
        attemptEvidence = {};
        attemptTick = {};
        if (closed && active)
            active->Close();
    }

    void SceneAnimationRuntime::State::PublishAttempt(const FixedStepContext &context) noexcept {
        RuntimeDispatchFacts facts;
        if (!ReadAttempt(context, facts))
            std::terminate();
        if (!attempting || !armed || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            std::terminate();
        if (facts.committedTick != facts.fixedTick || facts.committedAttempt != facts.fixedAttempt || facts.committedFrame != facts.frame ||
            facts.committedDuration != facts.fixedDuration)
            std::terminate();
        PublishActors();
        ClearAttempt();
    }

    void SceneAnimationRuntime::State::AbortAttempt(const FixedStepContext &context) noexcept {
        if (!OnOwner())
            return;
        RuntimeDispatchFacts facts;
        const auto status = context.dispatchEvidence.Read(config.expectedDispatch, RuntimePhase::FixedUpdate, facts);
        if (status != RuntimeDispatchStatus::Valid && status != RuntimeDispatchStatus::Retired)
            return;
        if (!attempting || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            return;
        // Retired does not expose facts. Only exact owner-thread evidence can authorize this attempt's rollback.
        AbortActors();
        ClearAttempt();
    }

    Result<void> SceneAnimationRuntime::OnFixedUpdate(const FixedStepContext &context) {
        const auto admitted = state_->BeginAttempt(context);
        if (admitted.HasError())
            return admitted;
        if (!state_->active || state_->active->closed)
            return Result<void>::Success();
        for (const auto &actor : state_->active->current->actors) {
            if (!actor)
                continue;
            const auto evaluated = state_->EvaluateActor(*actor, context);
            if (evaluated.HasError())
                return evaluated;
        }
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::PrepareFixedCommit(const FixedStepContext &context) {
        return state_->PrepareAttempt(context);
    }

    void SceneAnimationRuntime::CommitFixedUpdate(const FixedStepContext &context) noexcept {
        state_->PublishAttempt(context);
    }

    void SceneAnimationRuntime::AbortFixedUpdate(const FixedStepContext &context) noexcept {
        state_->AbortAttempt(context);
    }
}  // namespace Horo::Runtime
