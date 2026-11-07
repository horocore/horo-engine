#pragma once

/** @file SceneAnimationRuntime.h
 * @brief Canonical Scene lifecycle adapter over Animation's existing graph/asset owners and actual scheduler proof.
 */
#include "Horo/Animation/AnimationGraphInstance.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Runtime {
    /** @brief One invocation-scoped typed gameplay input snapshot; the graph copies it before evaluation returns. */
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
         * @return Bounded immutable input view valid until the caller's synchronous graph evaluation returns.
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
    };

    /** @brief Complete explicit host composition; absent or foreign scheduler authority is never synthesized. */
    struct SceneAnimationRuntimeConfig final {
        RuntimeDispatchSource expectedDispatch;
        Animation::AnimationRuntimeId runtimeOwner; /**< Nonwrapping identity issued by the actual cold host composition. */
        AssetCookTargetId target;
        CancellationToken ownerCancellation; /**< Actual host-owned lifetime token, never a retained Startup callback borrow. */
        SceneAnimationLimits limits;
        std::shared_ptr<const SceneAnimationInputSource> inputs;
    };

    /** @brief Non-owning immutable observation; no pose is claimed before an actual successful fixed commit. */
    struct SceneAnimationObservation final {
        EntityRef entity;
        Animation::AnimationAuthoringComponent authoring;
        Animation::AnimationInstanceHandle instance;
        Animation::AnimationPlaybackState state{Animation::AnimationPlaybackState::Ready};
        std::optional<Animation::RuntimeAnimationComponent> component;
    };

    /**
     * @brief The one Animation-owned Scene activation, structural and authoritative-tick adapter.
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
         * @details The successful steady-state path performs no publication, allocation or I/O. Rejected paths may allocate
         * canonical typed diagnostics. Forged/foreign/replayed dispatch fails before graph mutation.
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
        /** @brief Copies a genuine resident committed pose through the actual graph owner.
         * @param entity Exact active entity. @param pose Exact resident graph handle. @param output Complete caller-owned joint storage.
         * @return Existing allocation-free graph residency/owner/size outcome; failures preserve output.
         */
        [[nodiscard]] Animation::GraphInstanceOutcome CopyPose(EntityRef entity, const Animation::PoseHandle &pose,
                                                               std::span<Math::Transform> output) const;

    private:
        struct State;
        explicit SceneAnimationRuntime(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Runtime
