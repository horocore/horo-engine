#include "EditorProjectSceneHostOwner.h"
#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc Editor::EditorPlaySceneHost::ReadDecisionProblem */
    std::optional<Editor::EditorPlayDecisionProblem> ProjectPlaySceneHost::ReadDecisionProblem() const {
        const auto scene = scenes_ ? scenes_->ActiveScene() : std::nullopt;
        if (!scene || !ai_)
            return std::nullopt;
        auto captured = ai_->ReadDecisionFailureAtSafePoint(*scene);
        if (captured.HasError() || !captured.Value() || captured.Value()->tick > committedDecisionTick_)
            return std::nullopt;
        auto report = std::move(captured).Value();
        const auto &origin = report->origin;
        auto phase = origin.phase == AI::AiSchedulingFailurePhase::Evaluate ? Editor::EditorPlayDecisionPhase::Evaluate
                                                                            : Editor::EditorPlayDecisionPhase::Commit;
        if (origin.advisory)
            phase = Editor::EditorPlayDecisionPhase::TaskProvider;
        return Editor::EditorPlayDecisionProblem{.scene = scene->RuntimeId(),
                                                 .tick = report->tick,
                                                 .agent = origin.identity,
                                                 .phase = phase,
                                                 .error = std::move(report->error),
                                                 .entity = report->entity,
                                                 .graph = origin.asset,
                                                 .node = origin.node};
    }

    /** @copydoc Editor::EditorPlaySceneHost::AgentInspectionTick */
    std::uint64_t ProjectPlaySceneHost::AgentInspectionTick() const noexcept {
        return ai_ && !sceneClosed_ && !decisionTickPending_ && std::this_thread::get_id() == ownerLane_ ? committedDecisionTick_ : 0;
    }

    /** @copydoc Editor::EditorPlaySceneHost::SelectDecisionTraceOwner */
    void ProjectPlaySceneHost::SelectDecisionTraceOwner(const std::optional<Runtime::EntityRef> owner) noexcept {
        if (std::this_thread::get_id() != ownerLane_ || decisionTraceOwner_ == owner)
            return;
        decisionTraceOwner_ = owner;
        decisionDebug_.reset();
        committedDecisionTrace_.reset();
        pendingDecisionTrace_.reset();
    }

    /** @copydoc ProjectPlaySceneHost::SelectedDecisionAgent */
    std::optional<AI::AgentHandle> ProjectPlaySceneHost::SelectedDecisionAgent() const {
        const auto scene = decisionTraceOwner_ && ai_ && scenes_ ? scenes_->ActiveScene() : std::nullopt;
        if (!scene)
            return std::nullopt;
        const auto entity = scene->Get(*decisionTraceOwner_);
        if (entity.HasError() || !entity.Value().components || !entity.Value().components->aiAgent)
            return std::nullopt;
        const auto handle = ai_->CapabilityAtSafePoint().ResolveAgent(entity.Value().components->aiAgent->agent);
        return handle.HasValue() ? std::optional{handle.Value()} : std::nullopt;
    }

    /** @copydoc Editor::EditorPlaySceneHost::SetDecisionDebugView */
    void ProjectPlaySceneHost::SetDecisionDebugView(const std::optional<AI::AgentHandle> agent) {
        if (!agent && std::this_thread::get_id() == ownerLane_) {
            decisionDebug_.reset();
            return;
        }
        if (AgentInspectionTick() == 0 || !decisionDebugGranted_)
            return;
        if (decisionDebug_ && agent == decisionDebug_->agent)
            return;
        decisionDebug_.reset();
        if (agent && SelectedDecisionAgent() == agent)
            decisionDebug_.emplace(Editor::EditorPlayDecisionDebugState{.agent = *agent});
    }

    /** @copydoc Editor::EditorPlaySceneHost::ReadDecisionDebugState */
    std::optional<Editor::EditorPlayDecisionDebugState> ProjectPlaySceneHost::ReadDecisionDebugState() const noexcept {
        if (AgentInspectionTick() == 0 || !decisionDebug_)
            return std::nullopt;
        auto snapshot = *decisionDebug_;
        if (committedDecisionTrace_ && committedDecisionTrace_->Header().blackboard.agent == snapshot.agent)
            snapshot.trace = committedDecisionTrace_;
        return snapshot;
    }

    /** @copydoc ProjectPlaySceneHost::HasDecisionBreakpoint */
    bool ProjectPlaySceneHost::HasDecisionBreakpoint(const AI::DecisionStepTrace &trace,
                                                     const Editor::EditorPlayDecisionBreakpoint &breakpoint) {
        if (!breakpoint.graph.IsValid() || !breakpoint.node.IsValid())
            return false;
        return std::ranges::any_of(trace.Records(), [&](const AI::DecisionStepRecord &record) {
            return record.kind != AI::DecisionStepRecordKind::UtilityScore && breakpoint.graph == record.occurrence.graph &&
                   breakpoint.occurrence == record.occurrence.occurrence && breakpoint.node == record.node;
        });
    }

    /** @copydoc ProjectPlaySceneHost::SetDecisionBreakpoint */
    bool ProjectPlaySceneHost::SetDecisionBreakpoint(const Editor::EditorPlayDecisionDebugCommand &command) {
        if (!command.breakpoint || !committedDecisionTrace_ || committedDecisionTrace_->Header().blackboard.agent != command.agent ||
            !HasDecisionBreakpoint(*committedDecisionTrace_, *command.breakpoint))
            return false;
        decisionDebug_->breakpoint = command.breakpoint;
        decisionBreakpointPlan_ = committedDecisionTrace_->Header().planDigest;
        return true;
    }

    /** @copydoc Editor::EditorPlaySceneHost::ApplyDecisionDebugCommand */
    bool ProjectPlaySceneHost::ApplyDecisionDebugCommand(const Editor::EditorPlayDecisionDebugCommand &command) {
        if (AgentInspectionTick() == 0 || !decisionDebug_ || decisionDebug_->agent != command.agent ||
            SelectedDecisionAgent() != command.agent)
            return false;
        auto &state = *decisionDebug_;
        using enum Editor::EditorPlayDecisionDebugAction;
        switch (command.action) {
            case Step:
                state.stepPending = state.paused;
                return state.stepPending;
            case Pause:
            case Resume:
                state.paused = command.action == Pause;
                state.stepPending = false;
                return true;
            case SetBreakpoint:
                return SetDecisionBreakpoint(command);
            case ClearBreakpoint:
                state.breakpoint.reset();
                return true;
        }
        return false;
    }

    /** @copydoc ProjectPlaySceneHost::CommitDecisionDebugTick */
    void ProjectPlaySceneHost::CommitDecisionDebugTick() noexcept {
        if (!decisionDebug_)
            return;
        auto &state = *decisionDebug_;
        state.stepPending = false;
        if (!state.breakpoint || !committedDecisionTrace_ || committedDecisionTrace_->Header().tick != committedDecisionTick_)
            return;
        if (committedDecisionTrace_->Header().planDigest != decisionBreakpointPlan_) {
            state.breakpoint.reset();
            return;
        }
        if (HasDecisionBreakpoint(*committedDecisionTrace_, *state.breakpoint))
            state.paused = true;
    }

    /** @copydoc ProjectPlaySceneHost::ArmSelectedDecisionTrace */
    std::optional<AI::AgentHandle> ProjectPlaySceneHost::ArmSelectedDecisionTrace(const std::uint64_t tick) {
        const bool debugSample = decisionDebug_ && (decisionDebug_->stepPending || decisionDebug_->breakpoint.has_value());
        if (!debugSample && lastDecisionTraceTick_ != 0 && tick - lastDecisionTraceTick_ < Editor::EditorPlayAgentInspectionTickSpacing)
            return std::nullopt;
        const auto agent = SelectedDecisionAgent();
        if (!agent || ai_->ArmDecisionReplayAtSafePoint(*agent, tick).HasError())
            return std::nullopt;
        lastDecisionTraceTick_ = tick;
        return agent;
    }

    /** @copydoc ProjectPlaySceneHost::CaptureSelectedDecisionTrace */
    void ProjectPlaySceneHost::CaptureSelectedDecisionTrace(const AI::AgentHandle agent, const std::uint64_t tick) {
        if (const auto scene = scenes_->ActiveScene()) {
            auto trace = ai_->TakeDecisionTraceAtSafePoint(*scene, agent, tick);
            if (trace.HasValue()) {
                pendingDecisionTrace_ = std::move(trace).Value();
                return;
            }
        }
        ai_->DiscardDecisionReplayAtSafePoint(agent, tick);
    }

    /** @copydoc Editor::EditorPlaySceneHost::ReadAgentInspection */
    std::optional<Editor::EditorPlayAgentInspection> ProjectPlaySceneHost::ReadAgentInspection(const Runtime::EntityRef owner) const {
        const auto tick = AgentInspectionTick();
        const auto scene = tick != 0 && scenes_ ? scenes_->ActiveScene() : std::nullopt;
        if (!scene || owner.runtime != scene->RuntimeId())
            return std::nullopt;
        const auto entity = scene->Get(owner);
        if (entity.HasError() || !entity.Value().components || !entity.Value().components->aiAgent ||
            !entity.Value().components->aiController)
            return std::nullopt;
        const auto &components = *entity.Value().components;
        const auto handle = ai_->CapabilityAtSafePoint().ResolveAgent(components.aiAgent->agent);
        if (handle.HasError())
            return std::nullopt;
        try {
            std::array<AI::BlackboardKeyId, AI::MaximumBlackboardKeys> keys{};
            std::size_t count{};
            for (const auto &disclosed : inspectionKeys_)
                if (disclosed.schema == components.aiController->blackboardSchema)
                    keys[count++] = disclosed.key;
            auto snapshot = ai_->CaptureAgentInspectionAtSafePoint(*scene, handle.Value(), std::span{keys}.first(count));
            if (snapshot.HasError() || snapshot.Value().owner != owner || snapshot.Value().decisionTick != tick)
                return std::nullopt;
            return Editor::EditorPlayAgentInspection{owner,
                                                     handle.Value(),
                                                     tick,
                                                     std::make_shared<const AI::AiAgentInspectionSnapshot>(std::move(snapshot).Value()),
                                                     ReadAgentNavigation(owner),
                                                     committedDecisionTrace_ && decisionTraceOwner_ == owner &&
                                                             committedDecisionTrace_->Header().blackboard.agent == handle.Value()
                                                         ? committedDecisionTrace_
                                                         : nullptr};
        } catch (const std::bad_alloc &) {
            return std::nullopt;
        }
    }

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
        if (selection.inspectionKeys.size() > AI::MaximumBlackboardKeys ||
            std::ranges::any_of(selection.inspectionKeys, [](const auto &key) {
            return !key.schema.IsValid() || !key.key.IsValid();
        }))
            return Result<void>::Failure(MakeError(AI::AIErrors::BlackboardSchemaInvalid));
        pendingDecisionPolicy_.emplace(policy);
        decisionJobs_ = &jobs;
        return Result<void>::Success();
    }

    /** @copydoc Editor::EditorPlaySceneHost::ConfigureDecisionProviders */
    Result<void> ProjectPlaySceneHost::ConfigureDecisionProviders(const Runtime::RuntimeSceneDefinition &definition,
                                                                  const Editor::EditorPlayDecisionProviderSource &providerSource,
                                                                  const CancellationToken &cancellation) {
        if (!pendingDecisionPolicy_ || !decisionJobs_ || !providerSource.decision || sceneClosed_ ||
            std::this_thread::get_id() != ownerLane_)
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
        const auto providers = providerSource.decision();
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
        if (auto perception = PrepareDecisionPerception(policy, cooked.Value(), definition, providerSource); perception.HasError())
            return perception;
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        auto admitted = RegisterPresentationParticipants(policy, AI::AiSceneCookedDecisionExecution{*decisionJobs_, {}, {}, {}});
        if (admitted.HasValue()) {
            inspectionKeys_ = std::move(policy.decisions->inspectionKeys);
            decisionDebugGranted_ = policy.decisions->decisionDebugGranted;
        }
        return admitted;
    }

    /** @copydoc ProjectPlaySceneHost::RunDecisionScheduler */
    Result<void> ProjectPlaySceneHost::RunDecisionScheduler(AI::AiTaskScheduler &scheduler, const std::uint64_t tick) {
        if (decisionDebug_ && SelectedDecisionAgent() != decisionDebug_->agent)
            decisionDebug_.reset();
        const auto held =
            decisionDebug_ && decisionDebug_->paused && !decisionDebug_->stepPending ? std::optional{decisionDebug_->agent} : std::nullopt;
        const auto armed = held ? std::nullopt : ArmSelectedDecisionTrace(tick);
        const auto evaluated = scheduler.EvaluateAtDecisionWithPausedAgent(tick, held.value_or(AI::AgentHandle{}));
        const auto dispatched = evaluated.HasValue() ? scheduler.CommitAtIntentDispatch(tick) : evaluated;
        if (dispatched.HasError()) {
            if (armed)
                ai_->DiscardDecisionReplayAtSafePoint(*armed, tick);
            return Result<void>::Failure(dispatched.ErrorValue());
        }
        if (armed)
            CaptureSelectedDecisionTrace(*armed, tick);
        return Result<void>::Success();
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
        if (auto perception = PollDecisionPerception(tick, context.fixedDelta); perception.HasError())
            return Evaluation::Failure(perception.ErrorValue());
        if (auto decision = RunDecisionScheduler(*scheduler.Value(), tick); decision.HasError())
            return Evaluation::Failure(decision.ErrorValue());
        decisionTickPending_ = true;
        return Evaluation::Success(true);
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
