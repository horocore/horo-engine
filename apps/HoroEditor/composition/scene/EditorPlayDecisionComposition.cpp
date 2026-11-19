#include "EditorProjectSceneHostOwner.h"
#include "Horo/AI/AIErrors.h"

#include <algorithm>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc ProjectPlaySceneHost::PrepareDecisionSelection */
    Result<void> ProjectPlaySceneHost::PrepareDecisionSelection(const std::filesystem::path &root, JobSystem &jobs,
                                                                const Runtime::RuntimeSceneDefinition &definition,
                                                                const EditorProjectScenePolicy &policy) {
        const auto &selection = *policy.decisions;
        std::error_code error;
        if (identity_.value < Editor::EditorFirstPlayScene.value || !policy.cooked || !resources_.assets_ || policy.world ||
            policy.prefabGranted || !definition.ChildScenes().empty() || !selection.scene.IsValid() || selection.controllers.empty() ||
            selection.controllers.size() > AI::MaximumAiSceneAgents ||
            std::ranges::any_of(selection.controllers,
                                [](const auto &binding) {
            return binding.prefab.has_value();
        }) ||
            !std::filesystem::equivalent(root, selection.projectRoot, error) || error) {
            return Result<void>::Failure(
                MakeError(InvalidProjectScene,
                          "Decision Play requires an exact committed project Scene and initial controller selections."));
        }
        pendingDecisionPolicy_.emplace(policy);
        decisionJobs_ = &jobs;
        return Result<void>::Success();
    }

    /** @copydoc Editor::EditorPlaySceneHost::ConfigureDecisionProviders */
    Result<void> ProjectPlaySceneHost::ConfigureDecisionProviders(const Runtime::RuntimeSceneDefinition &definition,
                                                                  const Editor::EditorPlayDecisionProviderSource &providerSource,
                                                                  const CancellationToken &cancellation) {
        if (!pendingDecisionPolicy_ || !decisionJobs_ || !providerSource || sceneClosed_ || std::this_thread::get_id() != ownerLane_)
            return Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        auto policy = std::move(*pendingDecisionPolicy_);
        pendingDecisionPolicy_.reset();
        auto cooked = resources_.assets_->DecodeScene(policy.decisions->scene);
        if (cooked.HasError())
            return Result<void>::Failure(cooked.ErrorValue());
        if (auto matched = MatchCommittedPlayProjection(definition, cooked.Value(), InvalidProjectScene); matched.HasError())
            return matched;
        const auto providers = providerSource();
        if (providers.HasError())
            return Result<void>::Failure(providers.ErrorValue());
        const auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError())
            return Result<void>::Failure(limits.ErrorValue());
        const AI::DecisionCompilationEnvironment environment{.nodes = providers.Value()};
        auto selected = AdmitCookedSceneControllers(policy.aiControllers, cooked.Value(),
                                                    {*resources_.assets_, environment, policy.decisions->controllers, {}, {}},
                                                    limits.Value(), cancellation);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        policy.aiControllers = std::move(selected).Value();
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        return RegisterPresentationParticipants(policy, AI::AiSceneCookedDecisionExecution{*decisionJobs_, {}, {}, {}});
    }

    /** @copydoc Editor::EditorPlaySceneHost::EvaluateDecisionBeforeGameplay */
    Result<bool> ProjectPlaySceneHost::EvaluateDecisionBeforeGameplay(const Runtime::FixedStepContext &context) {
        using Evaluation = Result<bool>;
        if (!decisionJobs_)
            return Evaluation::Success(false);
        if (!ai_ || sceneClosed_ || decisionTickPending_ || committedDecisionTick_ == std::numeric_limits<std::uint64_t>::max() ||
            std::this_thread::get_id() != ownerLane_)
            return Evaluation::Failure(MakeError(AI::AIErrors::SchedulerPhaseInvalid));
        if (context.cancellation.IsCancellationRequested())
            return Evaluation::Failure(JobCancelled().ErrorValue());
        const auto scheduler = ai_->TaskSchedulerAtSafePoint();
        if (scheduler.HasError())
            return Evaluation::Failure(scheduler.ErrorValue());
        const auto tick = committedDecisionTick_ + 1;
        if (const auto evaluated = scheduler.Value()->EvaluateAtDecision(tick); evaluated.HasError())
            return Evaluation::Failure(evaluated.ErrorValue());
        if (const auto committed = scheduler.Value()->CommitAtIntentDispatch(tick); committed.HasError())
            return Evaluation::Failure(committed.ErrorValue());
        decisionTickPending_ = true;
        return Evaluation::Success(true);
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
