#include "AnimationGraphSceneCommitAccess.h"
#include "Horo/Foundation/JobSystem.h"
#include "SceneAnimationState.h"

#include <algorithm>
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
        if (const auto admission = Admit(true); admission.HasError())
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

    /** @copydoc SceneAnimationRuntime::State::CaptureInputs */
    Result<void> SceneAnimationRuntime::State::CaptureInputs(const FixedStepContext &context) const {
        if (!config.inputs)
            return Result<void>::Success();
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->playing)
                continue;
            auto snapshot = config.inputs->Read(actor->entity, context);
            if (snapshot.parameters.size() > actor->inputStorage.size())
                return Result<void>::Failure(A::DescribeGraphInstanceFailure({A::GraphInstanceFailure::LimitExceeded}));
            std::ranges::copy(snapshot.parameters, actor->inputStorage.begin());
            snapshot.parameters = std::span{actor->inputStorage}.first(snapshot.parameters.size());
            actor->capturedInput = snapshot;
        }
        return Result<void>::Success();
    }

    /** @copydoc SceneAnimationRuntime::State::EvaluateRange */
    Result<void> SceneAnimationRuntime::State::EvaluateRange(std::span<const std::shared_ptr<Actor>> actors,
                                                             const A::GraphEvaluationInput &batch, const CancellationToken &work) {
        for (const auto &actor : actors) {
            if (!actor || !actor->playing)
                continue;
            const auto &snapshot = actor->capturedInput;
            auto input = batch;
            input.rate = snapshot.playbackRate;
            input.parameters = snapshot.parameters;
            input.curveTime = snapshot.curveTime;
            input.cancellation = work;
            if (const auto outcome = A::Detail::GraphSceneCommitAccess::EvaluateIsolated(actor->graph, input, batch.cancellation);
                outcome.HasError())
                return Result<void>::Failure(A::DescribeGraphInstanceFailure(outcome));
            actor->staged = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc SceneAnimationRuntime::State::EvaluateActors */
    Result<void> SceneAnimationRuntime::State::EvaluateActors(const FixedStepContext &context) const {
        const auto &slots = active->current->actors;
        const A::GraphEvaluationInput batch{.tick = attemptTick,
                                            .delta = {context.fixedDelta.ToNanoseconds()},
                                            .cancellation = context.cancellation};
        if (!config.parallelEvaluation)
            return EvaluateRange(slots, batch, batch.cancellation);

        const auto playing = [](const auto &actor) {
            return actor && actor->playing;
        };
        const auto count = static_cast<std::size_t>(std::ranges::count_if(slots, playing));
        const auto &policy = *config.parallelEvaluation;
        auto &jobs = policy.jobs.get();
        const auto partitions = std::min(count, jobs.WorkerCount());
        if (partitions < 2)
            return EvaluateRange(slots, batch, batch.cancellation);

        const auto last = std::find_if(slots.rbegin(), slots.rend(), playing).base();
        const auto actors = std::span{slots}.first(static_cast<std::size_t>(last - slots.begin()));
        const auto width = (actors.size() + partitions - 1) / partitions;
        try {
            TaskGroup group(jobs, TaskGroupFailurePolicy::CollectAll, context.cancellation);
            for (std::size_t first = 0; first < actors.size(); first += width) {
                const auto slice = actors.subspan(first, std::min(width, actors.size() - first));
                auto submitted = group.Spawn({}, [slice, batch](const CancellationToken &cancellation) {
                    return EvaluateRange(slice, batch, cancellation);
                });
                if (submitted.HasError())
                    return Result<void>::Failure(std::move(submitted).ErrorValue());
            }
            return group.Join({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = policy.deadline});
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Capacity));
        }
    }

    Result<void> SceneAnimationRuntime::State::PrepareAttempt(const FixedStepContext &context) {
        if (RuntimeDispatchFacts facts; !ReadAttempt(context, facts))
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (const auto admission = Admit(); admission.HasError())
            return admission;
        if (!attempting || armed || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        if (auto prepared = ArmActors(); prepared.HasError())
            return prepared;
        armed = true;
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::State::ArmActors() const {
        if (!active || active->closed)
            return Result<void>::Success();
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            if (actor->pendingMotion && actor->pendingMotion->claimed &&
                actor->pendingMotion->result.disposition == SceneRootMotionDisposition::Unavailable)
                return Result<void>::Failure(
                    MakeError(A::AnimationErrors::GraphAdmissionRejected, "A claimed root motion request has no response."));
            const auto outcome = A::Detail::GraphSceneCommitAccess::Prepare(actor->graph, attemptTick);
            if (outcome.HasError())
                return Result<void>::Failure(A::DescribeGraphInstanceFailure(outcome));
        }
        return Result<void>::Success();
    }

    void SceneAnimationRuntime::State::PublishActors() const noexcept {
        if (!active || active->closed)
            return;
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            (void)A::Detail::GraphSceneCommitAccess::Publish(actor->graph);
            if (actor->pendingMotion) {
                actor->publishedMotion = actor->pendingMotion->result;
                actor->pendingMotion.reset();
            }
            actor->staged = false;
        }
    }

    void SceneAnimationRuntime::State::AbortActors() const noexcept {
        if (!active || active->closed)
            return;
        for (const auto &actor : active->current->actors) {
            if (!actor || !actor->staged)
                continue;
            if (actor->graph.Abort().HasError())
                std::terminate();
            actor->pendingMotion.reset();
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
        if (const auto status = context.dispatchEvidence.Read(config.expectedDispatch, RuntimePhase::FixedUpdate, facts);
            status != RuntimeDispatchStatus::Valid && status != RuntimeDispatchStatus::Retired)
            return;
        if (!attempting || !attemptEvidence.RefersToSameDispatch(context.dispatchEvidence))
            return;
        // Retired does not expose facts. Only exact owner-thread evidence can authorize this attempt's rollback.
        AbortActors();
        ClearAttempt();
    }

    Result<void> SceneAnimationRuntime::OnFixedUpdate(const FixedStepContext &context) {
        if (const auto admitted = state_->BeginAttempt(context); admitted.HasError())
            return admitted;
        if (!state_->active || state_->active->closed)
            return Result<void>::Success();
        if (const auto captured = state_->CaptureInputs(context); captured.HasError())
            return captured;
        if (const auto evaluated = state_->EvaluateActors(context); evaluated.HasError())
            return evaluated;
        return state_->StageRootMotion(context);
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
