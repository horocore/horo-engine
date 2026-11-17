#include "SceneAnimationState.h"

#include <exception>

namespace Horo::Runtime {
    namespace A = Animation;

    namespace {
        bool CanBindSource(const RuntimeDispatchSource &source) noexcept {
            const auto status = source.BindingStatus();
            // ValidateRun checks actual owner, retirement and exhaustion before reporting an active reservation.
            // Reentrant binds issuer identity only; every fixed operation still needs Read of its exact dispatch.
            return status == RuntimeDispatchStatus::Valid || status == RuntimeDispatchStatus::Reentrant;
        }

        bool ValidStoragePolicy(const SceneAnimationLimits &limits) noexcept {
            if (limits.maximumInstances == 0 || limits.maximumInstances > 16384)
                return false;
            if (limits.maximumInstanceStorageBytes == 0 || limits.maximumInstanceStorageBytes > 256ULL * 1024ULL * 1024ULL)
                return false;
            if (limits.maximumAggregateStorageBytes == 0 || limits.maximumAggregateStorageBytes > 1024ULL * 1024ULL * 1024ULL)
                return false;
            return limits.maximumInstances <= limits.maximumAggregateStorageBytes / limits.maximumInstanceStorageBytes;
        }

        bool ValidWorkPolicy(const SceneAnimationLimits &limits) noexcept {
            return limits.maximumInstructions > 0 && limits.maximumInstructions <= A::AnimationGraphHardLimits::EvaluationInstructions &&
                   limits.maximumJointOperations > 0 && limits.maximumJointOperations <= 64ULL * 1024ULL * 1024ULL;
        }

        A::GraphInstanceOutcome Rejected(A::GraphInstanceFailure failure) noexcept {
            A::GraphInstanceOutcome outcome;
            outcome.failure = failure;
            return outcome;
        }
    }  // namespace

    SceneAnimationRuntime::SceneAnimationRuntime(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    SceneAnimationRuntime::~SceneAnimationRuntime() {
        Shutdown();
    }

    Result<std::unique_ptr<SceneAnimationRuntime>> SceneAnimationRuntime::Create(SceneAnimationRuntimeConfig config) {
        using Return = Result<std::unique_ptr<SceneAnimationRuntime>>;
        if (!config.runtimeOwner.IsValid() || !config.target.IsValid() || !CanBindSource(config.expectedDispatch))
            return Return::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (!ValidStoragePolicy(config.limits) || !ValidWorkPolicy(config.limits))
            return Return::Failure(MakeError(A::AnimationAssetErrors::Capacity));
        if (config.ownerCancellation.IsCancellationRequested())
            return Return::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        try {
            return Return::Success(
                std::unique_ptr<SceneAnimationRuntime>(new SceneAnimationRuntime(std::make_shared<State>(std::move(config)))));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(A::AnimationAssetErrors::Capacity));
        }
    }

    Result<void> SceneAnimationRuntime::Startup(const CancellationToken &cancellation) {
        if (const auto admission = state_->Admit(true); admission.HasError())
            return admission;
        if (state_->started)
            return Result<void>::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Cancelled));
        state_->started = true;
        return Result<void>::Success();
    }

    Result<void> SceneAnimationRuntime::OnPhase(RuntimePhase, const FrameContext &) {
        return state_->Admit();
    }

    void SceneAnimationRuntime::Shutdown() noexcept {
        state_->Close();
    }

    Result<SceneAnimationObservation> SceneAnimationRuntime::Query(EntityRef entity) const {
        using Return = Result<SceneAnimationObservation>;
        if (const auto admission = state_->Admit(); admission.HasError())
            return Return::Failure(admission.ErrorValue());
        const auto actor = state_->Find(entity);
        if (!actor)
            return Return::Failure(MakeError(A::AnimationErrors::HandleStale));
        SceneAnimationObservation observation;
        observation.entity = actor->entity;
        observation.authoring = actor->authoring;
        observation.instance = actor->instance;
        observation.state = actor->playing ? A::AnimationPlaybackState::Playing : A::AnimationPlaybackState::Paused;
        if (const auto publication = actor->graph.Publication(); publication.currentPose.IsValid()) {
            const auto previous = publication.previousPose.IsValid() ? publication.previousPose : publication.currentPose;
            observation.component = A::RuntimeAnimationComponent{A::CurrentAnimationComponentContractVersion,
                                                                 actor->instance,
                                                                 actor->authoring.skeleton,
                                                                 actor->authoring.source,
                                                                 A::AnimationEvaluationDomain::AuthoritativeSimulation,
                                                                 observation.state,
                                                                 previous,
                                                                 publication.currentPose};
        }
        return Return::Success(std::move(observation));
    }

    Result<void> SceneAnimationRuntime::SetPlaying(EntityRef entity, bool playing) {
        if (const auto admission = state_->Admit(true); admission.HasError())
            return admission;
        const auto actor = state_->Find(entity);
        if (!actor)
            return Result<void>::Failure(MakeError(A::AnimationErrors::HandleStale));
        actor->playing = playing;
        return Result<void>::Success();
    }

    A::GraphInstanceOutcome SceneAnimationRuntime::QueueTrigger(EntityRef entity, A::GraphParameterId parameter) {
        using enum Horo::Animation::GraphInstanceFailure;
        if (!state_->OnOwner())
            return Rejected(ThreadViolation);
        if (state_->closed || state_->attempting)
            return Rejected(AdmissionRejected);
        if (state_->config.ownerCancellation.IsCancellationRequested())
            return Rejected(Cancelled);
        const auto actor = state_->Find(entity);
        if (!actor)
            return Rejected(HandleStale);
        return actor->graph.QueueTrigger(parameter);
    }

    A::GraphInstanceOutcome SceneAnimationRuntime::CopyPose(EntityRef entity, const A::PoseHandle &pose,
                                                            std::span<Math::Transform> output) const {
        if (!state_->OnOwner())
            return Rejected(A::GraphInstanceFailure::ThreadViolation);
        const auto actor = state_->Find(entity);
        if (!actor)
            return Rejected(A::GraphInstanceFailure::HandleStale);
        return actor->graph.CopyPose(pose, output);
    }
}  // namespace Horo::Runtime
