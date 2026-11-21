#pragma once

/** @file SceneAnimationRuntime.h
 * @brief Canonical Scene lifecycle adapter over Animation's existing graph/asset owners and actual scheduler proof.
 */
#include "Horo/Animation/AnimationGraphInstance.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <functional>

namespace Horo {
    class JobSystem;
}

namespace Horo::Runtime {
    /** @brief One invocation-scoped typed gameplay input snapshot copied by the Scene owner before graph evaluation. */
    struct SceneAnimationInputSnapshot final {
        std::span<const Animation::GraphParameterInput> parameters;
        Animation::GraphPlaybackRate playbackRate;
        Animation::AnimationTime curveTime;
    };

    /** @brief Explicit adjacent-owner input seam; no animation clock or parameter store is introduced here. */
    class SceneAnimationInputSource {
    public:
        virtual ~SceneAnimationInputSource() = default;
        /** @brief Borrows the actual immutable gameplay snapshot prepared before this exact attempted tick.
         * @param entity Exact Scene entity incarnation. @param context Actual callback, never retained.
         * @return Bounded immutable input view valid until the next Read or the end of the fixed callback.
         * @details Owner lane, allocation/I/O/callback-free; no mutation of this adapter or its graph owners.
         */
        [[nodiscard]] virtual SceneAnimationInputSnapshot Read(EntityRef entity, const FixedStepContext &context) const noexcept = 0;
    };

    /** @brief Whole-Scene cold admission and finite per-tick graph policies. */
    struct SceneAnimationLimits final {
        std::uint32_t maximumInstances{64};
        std::uint64_t maximumInstanceStorageBytes{4ULL * 1024ULL * 1024ULL};
        std::uint64_t maximumAggregateStorageBytes{256ULL * 1024ULL * 1024ULL};
        std::uint32_t maximumInstructions{Animation::AnimationGraphHardLimits::EvaluationInstructions};
        std::uint64_t maximumJointOperations{64ULL * 1024ULL * 1024ULL};
        Animation::AnimationClipLimits clip;
        Animation::GraphActionLimits actions;
        std::uint32_t maximumClipOccurrencesPerTick{256}; /**< Cold-reserved event capacity per active graph publication. */
    };

    /** @brief Optional host-provided CPU evaluation policy; the job system outlives the Scene adapter. */
    struct SceneAnimationParallelEvaluation final {
        std::reference_wrapper<JobSystem> jobs;
        Duration deadline;
    };

    /** @brief Complete explicit host composition; absent or foreign scheduler authority is never synthesized. */
    struct SceneAnimationRuntimeConfig final {
        RuntimeDispatchSource expectedDispatch;
        Animation::AnimationRuntimeId runtimeOwner; /**< Nonwrapping identity issued by the actual cold host composition. */
        AssetCookTargetId target;
        CancellationToken ownerCancellation; /**< Actual host-owned lifetime token, never a retained Startup callback borrow. */
        SceneAnimationLimits limits;
        std::shared_ptr<const SceneAnimationInputSource> inputs;
        Animation::AnimationEvaluationDomain evaluationDomain{
            Animation::AnimationEvaluationDomain::AuthoritativeSimulation}; /**< Fixed domain for the admitted graph and pose receipts. */
        std::optional<SceneAnimationParallelEvaluation> parallelEvaluation;
        bool enableRootMotionRequests{}; /**< Explicit host opt-in for authoritative same-attempt movement integration. */
    };

    /** @brief One root motion request addressed to an exact Scene entity and fixed attempt. */
    struct SceneRootMotionRequest final {
        EntityRef entity;
        Animation::RootMotionRequestId id;
        Animation::AnimationRootMotionDelta delta;
    };

    /** @brief Disposition committed by Animation after the same scheduler fence as the pose. */
    enum class SceneRootMotionDisposition : std::uint8_t {
        Unavailable,
        Rejected,
        Consumed
    };

    /** @brief Owned feedback for one historical root motion request. */
    struct SceneRootMotionResult final {
        SceneRootMotionRequest request;
        SceneRootMotionDisposition disposition{SceneRootMotionDisposition::Unavailable};
        Animation::AnimationRootMotionDelta applied{};   /**< Displacement in the request's starting root frame. */
        Animation::AnimationRootMotionDelta remaining{}; /**< Displacement from the applied pose to the requested endpoint. */
    };

    /** @brief Non-owning immutable observation; no pose is claimed before an actual successful fixed commit. */
    struct SceneAnimationObservation final {
        EntityRef entity;
        Animation::AnimationAuthoringComponent authoring;
        Animation::AnimationInstanceHandle instance;
        Animation::AnimationPlaybackState state{Animation::AnimationPlaybackState::Ready};
        std::optional<Animation::RuntimeAnimationComponent> component;
        std::optional<SceneRootMotionResult> rootMotion; /**< Latest result from this exact committed instance. */
    };

    /** @brief Stable attachment target that expires when its animation instance is replaced. */
    struct SceneAnimationSocketBinding final {
        EntityRef entity;                            /**< Exact active Scene entity. */
        Animation::AnimationInstanceHandle instance; /**< Exact animation incarnation. */
        Animation::SkeletonSocketId socket;          /**< Stable identity in the bound skeleton. */
    };

    /**
     * @brief The Animation-owned Scene activation, structural and fixed-tick adapter.
     * @details Creation, all callbacks/control operations and destruction belong to the creating owner lane. Shared private
     * state pins actual cold-prepared graph/assets until canonical Scene retirement. It owns no filesystem, cache, independent
     * scheduler, global clock, source-parser or gameplay state. Host composition evaluates exactly once between the actual
     * gameplay snapshot and native Physics; it must not also register a second evaluation path.
     */
    class SceneAnimationRuntime final : public RuntimeLifecycleParticipant {
    public:
        /** @brief Validates explicit host authority and finite policy, without source I/O or native activation.
         * @param config Actual host dispatch source, selected target, limits and optional owned gameplay snapshot source.
         * @return Prepared adapter or typed owner/policy/allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<SceneAnimationRuntime>> Create(SceneAnimationRuntimeConfig config);
        ~SceneAnimationRuntime() override;
        SceneAnimationRuntime(const SceneAnimationRuntime &) = delete;
        SceneAnimationRuntime &operator=(const SceneAnimationRuntime &) = delete;

        /** @brief Creates the actual typed activation/dependency-discovery participant sharing only this owner state.
         * @return Owned participant for the existing RuntimeSceneService; no registration or activation occurs here.
         */
        [[nodiscard]] std::unique_ptr<SceneActivationParticipant> MakeActivationParticipant();
        /** @brief Creates the actual Animation structural owner for the existing canonical Scene transaction.
         * @return Owned participant; missing active resources reject before mutation, never trigger loading here.
         */
        [[nodiscard]] std::unique_ptr<SceneStructuralParticipant> MakeStructuralParticipant();

        /** @brief Starts this exact owner after cancellation/admission checks. @param cancellation Callback-only host token.
         * @return Success or typed closed/foreign-owner/cancelled result; no work survives failed startup.
         */
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        /** @brief Observes canonical nonfixed phases without advancing authoritative animation.
         * @param phase Actual host phase. @param context Actual immutable frame callback. @return Typed lifecycle result.
         */
        [[nodiscard]] Result<void> OnPhase(RuntimePhase phase, const FrameContext &context) override;
        /** @brief Evaluates each playing instance once using the exact admitted fixed quantum and gameplay snapshot.
         * @param context Actual scheduler-issued attempted tick. @return Success with staged candidates or typed failure.
         * @details Serial steady-state evaluation allocates no memory. Opt-in parallel evaluation uses bounded Foundation dispatch
         * and joins every child before the owner can commit or abort. No evaluation path publishes poses or performs I/O.
         * Forged/foreign/replayed dispatch fails before graph mutation.
         */
        [[nodiscard]] Result<void> OnFixedUpdate(const FixedStepContext &context) override;
        /** @brief Validates all staged candidates through the existing Animation graph arm bridge.
         * @param context Same actual attempted dispatch. @return Success or typed failure before the global fence.
         */
        [[nodiscard]] Result<void> PrepareFixedCommit(const FixedStepContext &context) override;
        /** @brief Publishes every armed candidate after exact same-source scheduler commitment.
         * @param context Same actual committed dispatch; a broken invariant terminates rather than skipping publication.
         * @details No failure/cancellation/callback/allocation/I/O after this fence. Graph first-pose presentation uses that
         * actual current handle as both baseline endpoints; graph publication/history is unchanged.
         */
        void CommitFixedUpdate(const FixedStepContext &context) noexcept override;
        /** @brief Discards staged/armed candidates before the commit fence; never rolls back a committed pose.
         * @param context Actual aborted attempt, including an attempt that did not reach this owner.
         */
        void AbortFixedUpdate(const FixedStepContext &context) noexcept override;
        /** @brief Closes admission and retires graph owners before assets; safe repeatedly.
         * @details An already staged attempt remains owned by the scheduler until its exact commit/abort callback.
         */
        void Shutdown() noexcept override;

        /** @brief Reads one exact active entity; returned values retain no live pose/Scene pointer.
         * @param entity Exact generation-qualified owner. @return Copied observation or typed stale/owner failure.
         */
        [[nodiscard]] Result<SceneAnimationObservation> Query(EntityRef entity) const;
        /** @brief Claims one pending request for the host's movement owner.
         * @param entity Exact active entity. @param context Current fixed dispatch, after Animation evaluation.
         * @return One owned request, no request for an actor without staged motion, or a typed admission failure.
         * @details A second claim for the same attempt fails. Each claim requires a response before fixed preparation.
         */
        [[nodiscard]] Result<std::optional<SceneRootMotionRequest>> ClaimRootMotion(EntityRef entity, const FixedStepContext &context);
        /** @brief Stages movement feedback for one previously claimed request.
         * @param entity Exact active entity. @param request Identity returned by ClaimRootMotion for that entity.
         * @param applied Motion actually applied in the request frame, or empty for rejection.
         * @param context Current fixed dispatch. @return Success or a typed stale, duplicate, malformed or admission failure.
         * @details The host must stage movement through the same commit fence. An aborted attempt retires all feedback.
         */
        [[nodiscard]] Result<void> ResolveRootMotion(EntityRef entity, const Animation::RootMotionRequestId &request,
                                                     const std::optional<Animation::AnimationRootMotionDelta> &applied,
                                                     const FixedStepContext &context);
        /** @brief Changes component play intent only at an unarmed owner control boundary.
         * @param entity Exact active entity. @param playing True plays, false holds all graph state until resumed.
         * @return Success or typed stale/lifecycle/owner rejection. Host pause/step remains scheduler-owned.
         */
        [[nodiscard]] Result<void> SetPlaying(EntityRef entity, bool playing);
        /** @brief Delegates a typed trigger to the actual graph owner without a parallel queue.
         * @param entity Exact active entity. @param parameter Actual declared trigger ID.
         * @return Existing allocation-free graph outcome, including stale/thread/lifecycle/type errors.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome QueueTrigger(EntityRef entity, Animation::GraphParameterId parameter);
        /** @brief Stages a typed scalar for the exact active graph's next committed tick.
         * @param entity Exact active entity. @param parameter Declared scalar and its typed value.
         * @return Allocation-free graph outcome or Scene admission failure.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome QueueParameter(EntityRef entity, const Animation::GraphParameterInput &parameter);
        /** @brief Queues an action on an authoritative Scene graph at the owner control boundary.
         * @param entity Exact active entity. @param request Actual authored action and queue policy.
         * @return Existing graph command outcome with a handle on admission.
         */
        [[nodiscard]] Animation::GraphActionCommandOutcome QueueAction(EntityRef entity, const Animation::GraphActionRequest &request);
        /** @brief Queues cancellation of a retained authority action.
         * @param entity Exact active entity. @param handle Its live action handle. @param policy Authored fade or immediate cut.
         * @return Existing graph command outcome.
         */
        [[nodiscard]] Animation::GraphActionCommandOutcome CancelAction(EntityRef entity, const Animation::GraphActionHandle &handle,
                                                                        Animation::GraphActionStopPolicy policy);
        /** @brief Releases the authority graph's retained terminal record.
         * @param entity Exact active entity. @param handle Its terminal action.
         * @return Existing graph command outcome.
         */
        [[nodiscard]] Animation::GraphActionCommandOutcome ReleaseActionResult(EntityRef entity,
                                                                               const Animation::GraphActionHandle &handle);
        /** @brief Reads a retained authority action at the owner control boundary.
         * @param entity Exact active entity. @param handle Exact request identity for that entity.
         * @return Copied request fact or typed admission/handle failure.
         */
        [[nodiscard]] Animation::GraphActionQueryOutcome QueryAction(EntityRef entity, const Animation::GraphActionHandle &handle) const;
        /** @brief Copies the typed parameter block of an exact resident committed pose.
         * @param entity Exact active entity. @param pose Exact resident handle.
         * @param output Caller storage matching the number of declared graph parameters.
         * @return Typed owner/lifecycle/residency/size outcome. Failure preserves output.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyParameters(EntityRef entity, const Animation::PoseHandle &pose,
                                                                     std::span<Animation::GraphParameterInput> output) const;
        /** @brief Copies state-machine lifecycle events from one resident committed pose.
         * @param entity Exact active entity. @param pose Exact resident handle.
         * @param output Caller storage for the complete event block. @param written Receives the count on success.
         * @return Typed owner/lifecycle/residency/size outcome. Failure preserves output and count.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyStateEvents(EntityRef entity, const Animation::PoseHandle &pose,
                                                                      std::span<Animation::GraphStateEvent> output,
                                                                      std::uint32_t &written) const;
        /** @brief Copies clip occurrences from one resident committed pose without consuming them.
         * @param entity Exact active entity. @param pose Exact resident handle.
         * @param output Caller storage for the complete occurrence block. @param written Receives the count on success.
         * @return Typed owner/lifecycle/residency/size outcome. Failure preserves output and count.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyClipEvents(EntityRef entity, const Animation::PoseHandle &pose,
                                                                     std::span<Animation::AnimationEventOccurrence> output,
                                                                     std::uint32_t &written) const;
        /** @brief Copies a genuine resident committed pose through the actual graph owner.
         * @param entity Exact active entity. @param pose Exact resident graph handle. @param output Complete caller-owned joint storage.
         * @return Existing allocation-free graph residency/owner/size outcome; failures preserve output.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyPose(EntityRef entity, const Animation::PoseHandle &pose,
                                                               std::span<Math::Transform> output) const;

        /** @brief Reads a committed joint transform in the animated entity's model space.
         * @param entity Exact active entity. @param pose Current or previous resident handle.
         * @param joint Stable skeleton-local joint identity.
         * @return Value matrix or a typed owner, lifecycle, residency, or joint error.
         * @pre Invoke on the Scene owner lane. Successful reads perform no allocation.
         */
        [[nodiscard]] Result<Math::Mat4> JointModelTransform(EntityRef entity, const Animation::PoseHandle &pose,
                                                             Animation::JointId joint) const;

        /** @brief Binds an attachment to an active actor without depending on its current pose.
         * @param entity Exact active entity. @param socket Stable skeleton-local socket identity.
         * @return Value binding, or a typed owner, lifecycle, entity, or missing socket error.
         * @pre Invoke on the Scene owner lane.
         */
        [[nodiscard]] Result<SceneAnimationSocketBinding> BindSocket(EntityRef entity, Animation::SkeletonSocketId socket) const;

        /** @brief Reads a bound socket in the animated entity's model space.
         * @param binding Attachment bound to the exact current actor incarnation.
         * @param pose Current or previous resident handle for the bound actor.
         * @return Value matrix or a typed lifecycle, residency, binding, or socket error.
         * @pre Invoke on the Scene owner lane. Successful reads perform no allocation.
         */
        [[nodiscard]] Result<Math::Mat4> SocketModelTransform(const SceneAnimationSocketBinding &binding,
                                                              const Animation::PoseHandle &pose) const;

        /** @brief Copies action slot observations through a resident committed Scene pose.
         * @param entity Exact active entity. @param pose Exact resident handle. @param output Caller-owned slot storage.
         * @param written Complete slot count or zero on failure.
         * @return Existing allocation-free graph residency/owner/size outcome.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyActionSlots(EntityRef entity, const Animation::PoseHandle &pose,
                                                                      std::span<Animation::GraphActionSlotObservation> output,
                                                                      std::uint32_t &written) const;

    private:
        struct State;
        /** @brief Resolves the graph at the Scene's owner-lane parameter admission boundary. */
        [[nodiscard]] std::pair<Animation::AnimationGraphInstance *, Animation::GraphInstanceFailure> ResolveParameterGraph(
            EntityRef entity) const noexcept;
        /** @brief Resolves the mutable authoritative graph only on the idle owner lane. */
        [[nodiscard]] std::pair<Animation::AnimationGraphInstance *, Animation::GraphActionFailure> ResolveActionGraph(
            EntityRef entity) const noexcept;
        explicit SceneAnimationRuntime(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Runtime
