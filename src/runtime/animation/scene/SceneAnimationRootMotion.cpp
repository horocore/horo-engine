#include "AnimationGraphSceneCommitAccess.h"
#include "SceneAnimationState.h"

namespace Horo::Runtime {
    namespace A = Animation;

    namespace {
        /** @brief Derives the rigid remainder after the applied motion without mutating the pending receipt on failure. */
        Result<void> RecordAppliedMotion(SceneRootMotionResult &result, const A::AnimationRootMotionDelta &applied) {
            const auto rotation = applied.rotation.TryNormalized();
            const auto difference = result.request.delta.translation - applied.translation;
            if (rotation.HasError() || !Math::IsFinite(difference))
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            const auto inverse = rotation.Value().Inverse();
            const auto remaining = inverse.Rotate(difference);
            if (!Math::IsFinite(remaining))
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            result.applied = {applied.translation, rotation.Value()};
            result.remaining = {remaining, (inverse * result.request.delta.rotation).Normalized()};
            result.disposition = SceneRootMotionDisposition::Consumed;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SceneAnimationRuntime::State::StageRootMotion */
    Result<void> SceneAnimationRuntime::State::StageRootMotion(const FixedStepContext &context) const {
        if (!config.enableRootMotionRequests)
            return Result<void>::Success();
        // The actual scheduler never reuses an attempt number, even when the same tick is retried.
        const auto generation = A::RootMotionGeneration::Create(context.attemptNumber);
        if (generation.HasError())
            return Result<void>::Failure(generation.ErrorValue());
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged || actor->authoring.rootMotion != A::RootMotionPolicy::RequestCharacterMovement)
                continue;
            A::AnimationRootMotionDelta delta;
            if (const auto observed = A::Detail::GraphSceneCommitAccess::ReadRootMotion(actor->graph, attemptTick, delta);
                observed.HasError())
                return Result<void>::Failure(A::DescribeGraphInstanceFailure(observed));
            actor->pendingMotion = PendingRootMotion{
                SceneRootMotionResult{.request = {actor->entity, {actor->instance, attemptTick, generation.Value()}, delta},
                                      .remaining = delta}};
        }
        return Result<void>::Success();
    }

    /** @copydoc SceneAnimationRuntime::State::AdmitMotionCallback */
    Result<void> SceneAnimationRuntime::State::AdmitMotionCallback(const FixedStepContext &context) const {
        if (const auto admitted = Admit(); admitted.HasError())
            return admitted;
        if (RuntimeDispatchFacts facts;
            !attempting || armed || !ReadAttempt(context, facts) || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        return Result<void>::Success();
    }

    /** @copydoc SceneAnimationRuntime::ClaimRootMotion */
    Result<std::optional<SceneRootMotionRequest>> SceneAnimationRuntime::ClaimRootMotion(EntityRef entity,
                                                                                         const FixedStepContext &context) {
        using Return = Result<std::optional<SceneRootMotionRequest>>;
        if (const auto admitted = state_->AdmitMotionCallback(context); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        const auto actor = state_->Find(entity);
        if (!actor)
            return Return::Failure(MakeError(A::AnimationErrors::HandleStale));
        auto &pending = actor->pendingMotion;
        if (!pending.has_value())
            return Return::Success(std::nullopt);
        if (pending->claimed)
            return Return::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        pending->claimed = true;
        return Return::Success(pending->result.request);
    }

    /** @copydoc SceneAnimationRuntime::ResolveRootMotion */
    Result<void> SceneAnimationRuntime::ResolveRootMotion(EntityRef entity, const A::RootMotionRequestId &request,
                                                          const std::optional<A::AnimationRootMotionDelta> &applied,
                                                          const FixedStepContext &context) {
        if (const auto admitted = state_->AdmitMotionCallback(context); admitted.HasError())
            return admitted;
        const auto actor = state_->Find(entity);
        if (!actor || !actor->pendingMotion.has_value() || actor->pendingMotion->result.request.id != request)
            return Result<void>::Failure(MakeError(A::AnimationErrors::HandleStale));
        auto &pending = *actor->pendingMotion;
        if (!pending.claimed || pending.result.disposition != SceneRootMotionDisposition::Unavailable)
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (applied.has_value())
            return RecordAppliedMotion(pending.result, *applied);
        pending.result.disposition = SceneRootMotionDisposition::Rejected;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
