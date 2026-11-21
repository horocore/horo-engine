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
                   limits.maximumJointOperations > 0 && limits.maximumJointOperations <= 64ULL * 1024ULL * 1024ULL &&
                   limits.maximumClipOccurrencesPerTick > 0 &&
                   limits.maximumClipOccurrencesPerTick <= A::AnimationNotifyHardLimits::OccurrencesPerTick;
        }

        /** @brief Movement requests require the actual authoritative Scene domain. */
        bool ValidMotionPolicy(const SceneAnimationRuntimeConfig &config) noexcept {
            return !config.enableRootMotionRequests || config.evaluationDomain == A::AnimationEvaluationDomain::AuthoritativeSimulation;
        }

        /** @brief Checks explicit host identity and domain before any Scene owner allocation. */
        bool ValidComposition(const SceneAnimationRuntimeConfig &config) noexcept {
            return config.runtimeOwner.IsValid() && config.target.IsValid() && CanBindSource(config.expectedDispatch) &&
                   config.evaluationDomain < A::AnimationEvaluationDomain::Count && ValidMotionPolicy(config);
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
        if (!ValidComposition(config))
            return Return::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        if (!ValidStoragePolicy(config.limits) || !ValidWorkPolicy(config.limits))
            return Return::Failure(MakeError(A::AnimationAssetErrors::Capacity));
        if (config.parallelEvaluation && config.parallelEvaluation->deadline <= Duration{})
            return Return::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
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
        observation.rootMotion = actor->publishedMotion;
        if (const auto publication = actor->graph.Publication(); publication.currentPose.IsValid()) {
            const auto previous = publication.previousPose.IsValid() ? publication.previousPose : publication.currentPose;
            observation.component = A::RuntimeAnimationComponent{A::CurrentAnimationComponentContractVersion,
                                                                 actor->instance,
                                                                 actor->authoring.skeleton,
                                                                 actor->authoring.source,
                                                                 state_->config.evaluationDomain,
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

    /** @copydoc SceneAnimationRuntime::ResolveParameterGraph */
    std::pair<A::AnimationGraphInstance *, A::GraphInstanceFailure> SceneAnimationRuntime::ResolveParameterGraph(
        EntityRef entity) const noexcept {
        using enum A::GraphInstanceFailure;
        if (!state_->OnOwner())
            return {nullptr, ThreadViolation};
        if (state_->closed || state_->attempting)
            return {nullptr, AdmissionRejected};
        if (state_->config.ownerCancellation.IsCancellationRequested())
            return {nullptr, Cancelled};
        const auto actor = state_->Find(entity);
        if (!actor)
            return {nullptr, HandleStale};
        return {&actor->graph, None};
    }

    /** @copydoc SceneAnimationRuntime::QueueTrigger */
    A::GraphInstanceOutcome SceneAnimationRuntime::QueueTrigger(EntityRef entity, A::GraphParameterId parameter) {
        const auto [graph, failure] = ResolveParameterGraph(entity);
        return graph ? graph->QueueTrigger(parameter) : Rejected(failure);
    }

    /** @copydoc SceneAnimationRuntime::QueueParameter */
    A::GraphInstanceOutcome SceneAnimationRuntime::QueueParameter(EntityRef entity, const A::GraphParameterInput &parameter) {
        const auto [graph, failure] = ResolveParameterGraph(entity);
        return graph ? graph->QueueParameter(parameter) : Rejected(failure);
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

    /** @copydoc SceneAnimationRuntime::ResolveActionGraph */
    std::pair<A::AnimationGraphInstance *, A::GraphActionFailure> SceneAnimationRuntime::ResolveActionGraph(
        EntityRef entity) const noexcept {
        using enum A::GraphActionFailure;
        if (!state_->OnOwner())
            return {nullptr, ThreadViolation};
        if (state_->closed || state_->config.ownerCancellation.IsCancellationRequested() ||
            state_->config.evaluationDomain != A::AnimationEvaluationDomain::AuthoritativeSimulation)
            return {nullptr, Closed};
        if (state_->attempting)
            return {nullptr, EvaluationPending};
        const auto actor = state_->Find(entity);
        return actor ? std::pair{&actor->graph, None} : std::pair<A::AnimationGraphInstance *, A::GraphActionFailure>{nullptr, HandleStale};
    }

    /** @copydoc SceneAnimationRuntime::QueueAction */
    A::GraphActionCommandOutcome SceneAnimationRuntime::QueueAction(EntityRef entity, const A::GraphActionRequest &request) {
        const auto [graph, failure] = ResolveActionGraph(entity);
        return graph ? graph->QueueAction(request) : A::GraphActionCommandOutcome{failure};
    }

    /** @copydoc SceneAnimationRuntime::CancelAction */
    A::GraphActionCommandOutcome SceneAnimationRuntime::CancelAction(EntityRef entity, const A::GraphActionHandle &handle,
                                                                     A::GraphActionStopPolicy policy) {
        const auto [graph, failure] = ResolveActionGraph(entity);
        return graph ? graph->CancelAction(handle, policy) : A::GraphActionCommandOutcome{failure};
    }

    /** @copydoc SceneAnimationRuntime::ReleaseActionResult */
    A::GraphActionCommandOutcome SceneAnimationRuntime::ReleaseActionResult(EntityRef entity, const A::GraphActionHandle &handle) {
        const auto [graph, failure] = ResolveActionGraph(entity);
        return graph ? graph->ReleaseActionResult(handle) : A::GraphActionCommandOutcome{failure};
    }

    /** @copydoc SceneAnimationRuntime::QueryAction */
    A::GraphActionQueryOutcome SceneAnimationRuntime::QueryAction(EntityRef entity, const A::GraphActionHandle &handle) const {
        const auto [graph, failure] = ResolveActionGraph(entity);
        return graph ? graph->QueryAction(handle) : A::GraphActionQueryOutcome{failure};
    }

    /** @copydoc SceneAnimationRuntime::State::ResolveObservationActor */
    std::pair<const SceneAnimationRuntime::State::Actor *, A::GraphInstanceFailure> SceneAnimationRuntime::State::ResolveObservationActor(
        EntityRef entity) const noexcept {
        using enum A::GraphInstanceFailure;
        if (!OnOwner())
            return {nullptr, ThreadViolation};
        if (closed)
            return {nullptr, AdmissionRejected};
        if (config.ownerCancellation.IsCancellationRequested())
            return {nullptr, Cancelled};
        const auto actor = Find(entity);
        if (!actor)
            return {nullptr, HandleStale};
        return {actor.get(), None};
    }

    /** @copydoc SceneAnimationRuntime::JointModelTransform */
    Result<Math::Mat4> SceneAnimationRuntime::JointModelTransform(const EntityRef entity, const A::PoseHandle &pose,
                                                                  const A::JointId joint) const {
        const auto [actor, failure] = state_->ResolveObservationActor(entity);
        if (!actor)
            return Result<Math::Mat4>::Failure(A::DescribeGraphInstanceFailure(Rejected(failure)));
        return actor->graph.JointModelTransform(pose, joint);
    }

    /** @copydoc SceneAnimationRuntime::BindSocket */
    Result<SceneAnimationSocketBinding> SceneAnimationRuntime::BindSocket(const EntityRef entity, const A::SkeletonSocketId socket) const {
        const auto [actor, failure] = state_->ResolveObservationActor(entity);
        if (!actor)
            return Result<SceneAnimationSocketBinding>::Failure(A::DescribeGraphInstanceFailure(Rejected(failure)));
        if (!actor->assets.skeleton->FindSocket(socket))
            return Result<SceneAnimationSocketBinding>::Failure(MakeError(A::AnimationErrors::PoseSocketMissing));
        return Result<SceneAnimationSocketBinding>::Success({entity, actor->instance, socket});
    }

    /** @copydoc SceneAnimationRuntime::SocketModelTransform */
    Result<Math::Mat4> SceneAnimationRuntime::SocketModelTransform(const SceneAnimationSocketBinding &binding,
                                                                   const A::PoseHandle &pose) const {
        const auto [actor, failure] = state_->ResolveObservationActor(binding.entity);
        if (!actor)
            return Result<Math::Mat4>::Failure(A::DescribeGraphInstanceFailure(Rejected(failure)));
        if (!binding.instance.IsValid() || !binding.socket.IsValid())
            return Result<Math::Mat4>::Failure(MakeError(A::AnimationErrors::HandleMalformed));
        if (binding.instance != actor->instance)
            return Result<Math::Mat4>::Failure(MakeError(A::AnimationErrors::HandleStale));
        return actor->graph.SocketModelTransform(pose, binding.socket);
    }

    /** @copydoc SceneAnimationRuntime::CopyParameters */
    A::GraphInstanceOutcome SceneAnimationRuntime::CopyParameters(EntityRef entity, const A::PoseHandle &pose,
                                                                  std::span<A::GraphParameterInput> output) const {
        const auto [actor, failure] = state_->ResolveObservationActor(entity);
        return actor ? actor->graph.CopyParameters(pose, output) : Rejected(failure);
    }

    /** @copydoc SceneAnimationRuntime::CopyStateEvents */
    A::GraphInstanceOutcome SceneAnimationRuntime::CopyStateEvents(EntityRef entity, const A::PoseHandle &pose,
                                                                   std::span<A::GraphStateEvent> output, std::uint32_t &written) const {
        const auto [actor, failure] = state_->ResolveObservationActor(entity);
        return actor ? actor->graph.CopyStateEvents(pose, output, written) : Rejected(failure);
    }

    /** @copydoc SceneAnimationRuntime::CopyClipEvents */
    A::GraphInstanceOutcome SceneAnimationRuntime::CopyClipEvents(EntityRef entity, const A::PoseHandle &pose,
                                                                  std::span<A::AnimationEventOccurrence> output,
                                                                  std::uint32_t &written) const {
        const auto [actor, failure] = state_->ResolveObservationActor(entity);
        return actor ? actor->graph.CopyClipEvents(pose, output, written) : Rejected(failure);
    }

    /** @copydoc SceneAnimationRuntime::CopyActionSlots */
    A::GraphInstanceOutcome SceneAnimationRuntime::CopyActionSlots(EntityRef entity, const A::PoseHandle &pose,
                                                                   std::span<A::GraphActionSlotObservation> output,
                                                                   std::uint32_t &written) const {
        written = 0;
        if (!state_->OnOwner())
            return Rejected(A::GraphInstanceFailure::ThreadViolation);
        const auto actor = state_->Find(entity);
        if (!actor)
            return Rejected(A::GraphInstanceFailure::HandleStale);
        return actor->graph.CopyActionSlots(pose, output, written);
    }
}  // namespace Horo::Runtime
