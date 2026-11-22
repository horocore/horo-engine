#include "EditorProjectSceneHostOwner.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Application/ProjectAiSettings.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    namespace {
        /** @brief Authoritative scheduler settings and the effective development grant for one Play startup. */
        struct EditorDecisionPolicy final {
            AI::AiTaskSchedulerSettings scheduler;
            bool debugGranted{};
            std::size_t perceptionWorkUnitsLimit{AI::AiTaskSchedulerSettings::HardWorkUnits};
            bool schedulerModeAuthored{};
            std::size_t queryWorkUnitsLimit{AI::AiTaskSchedulerSettings::HardWorkUnits};
            std::optional<AI::QueryResultCacheLimits> queryCacheLimits;
            bool queryInspectionGranted{};
            std::optional<ProjectLocalPolicySettings> localPolicy;
        };

        /** @brief Resolves the selected project's policy before any provider admission. */
        Result<EditorDecisionPolicy> CaptureEditorDecisionPolicy(const EditorPlayDecisionSelection &selection) {
            using Policy = Result<EditorDecisionPolicy>;
            const auto source = LoadProjectSourceDocument(selection.projectRoot);
            if (source.HasError())
                return Policy::Failure(source.ErrorValue());
            const auto resolved = ResolveProjectAiSettings(source.Value());
            if (resolved.HasError())
                return Policy::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                return Policy::Success({{}, selection.decisionDebugGranted});
            const auto &settings = *resolved.Value();
            return Policy::Success({settings.Scheduler(),
                                    selection.decisionDebugGranted &&
                                        settings.AllowsDebug(AiProjectRuntimeKind::Development, AiProjectDebugOperation::LiveMutation),
                                    settings.PerceptionWorkUnitsLimit(), settings.HasAuthoredSchedulerMode(),
                                    settings.QueryWorkUnitsLimit(), settings.QueryCacheLimits(),
                                    settings.AllowsDebug(AiProjectRuntimeKind::Development, AiProjectDebugOperation::ReplayCapture),
                                    settings.LocalPolicy()});
        }
    }  // namespace

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

    Editor::EditorPlayQueryInspection ProjectPlaySceneHost::InspectQuery(
        std::optional<Editor::EditorPlayQueryInspectionSelection> selection) {
        if (!decisionQueries_ || std::this_thread::get_id() != ownerLane_)
            return {};
        const auto scene = selection && queryInspectionGranted_ && !sceneClosed_ ? scenes_->ActiveScene() : std::nullopt;
        std::optional<SceneQueryInspectionSelection> resolved;
        if (scene && scene->IsCurrent() && scene->RuntimeId() == selection->owner.runtime) {
            const auto entity = scene->Get(selection->owner);
            if (entity.HasValue() && entity.Value().components && entity.Value().components->aiAgent) {
                const auto agent = ai_->CapabilityAtSafePoint().ResolveAgent(entity.Value().components->aiAgent->agent);
                if (agent.HasValue())
                    resolved.emplace(agent.Value(), selection->query, selection->cooked);
            }
        }
        if (!decisionQueries_->SelectInspection(resolved))
            resolved.reset();
        const auto structure = resolved ? scene->StructuralRevision() : 0;
        if (liveQueryInspection_ != resolved || queryInspectionStructure_ != structure)
            committedQueryInspection_.reset();
        liveQueryInspection_ = resolved;
        queryInspectionStructure_ = structure;
        return {resolved.has_value(), committedQueryInspection_};
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

    std::optional<AI::AgentHandle> ProjectPlaySceneHost::ResolveInspectionAgent(const Runtime::RuntimeEntityView &entity) const {
        if (!entity.components || !entity.components->aiAgent || !entity.components->aiController)
            return std::nullopt;
        const auto runtime = ai_->CapabilityAtSafePoint();
        const auto handle = runtime.ResolveAgent(entity.components->aiAgent->agent);
        if (handle.HasError())
            return std::nullopt;
        const auto owner = runtime.ResolveAgentOwner(handle.Value());
        return owner.HasValue() && owner.Value() == entity.entity ? std::optional{handle.Value()} : std::nullopt;
    }

    /** @copydoc Editor::EditorPlaySceneHost::IsAgentInspectionCurrent */
    bool ProjectPlaySceneHost::IsAgentInspectionCurrent(const Runtime::EntityRef owner, const AI::AgentHandle agent) const {
        const auto scene = AgentInspectionTick() != 0 && scenes_ ? scenes_->ActiveScene() : std::nullopt;
        if (!scene || !agent.IsValid() || owner.runtime != scene->RuntimeId())
            return false;
        const auto entity = scene->Get(owner);
        if (entity.HasError() || !entity.Value().components || !entity.Value().components->aiAgent ||
            !entity.Value().components->aiController)
            return false;
        const auto liveOwner = ai_->CapabilityAtSafePoint().ResolveAgentOwner(agent);
        return liveOwner.HasValue() && liveOwner.Value() == owner;
    }

    /** @copydoc Editor::EditorPlaySceneHost::ReadAgentInspection */
    std::optional<Editor::EditorPlayAgentInspection> ProjectPlaySceneHost::ReadAgentInspection(const Runtime::EntityRef owner) const {
        const auto tick = AgentInspectionTick();
        const auto scene = tick != 0 && scenes_ ? scenes_->ActiveScene() : std::nullopt;
        if (!scene || owner.runtime != scene->RuntimeId())
            return std::nullopt;
        const auto entity = scene->Get(owner);
        const auto handle = entity.HasValue() ? ResolveInspectionAgent(entity.Value()) : std::nullopt;
        if (!handle)
            return std::nullopt;
        const auto &components = *entity.Value().components;
        try {
            std::array<AI::BlackboardKeyId, AI::MaximumBlackboardKeys> keys{};
            std::size_t count{};
            for (const auto &disclosed : inspectionKeys_)
                if (disclosed.schema == components.aiController->blackboardSchema)
                    keys[count++] = disclosed.key;
            auto snapshot = ai_->CaptureAgentInspectionAtSafePoint(*scene, *handle, std::span{keys}.first(count));
            if (snapshot.HasError() || snapshot.Value().owner != owner || snapshot.Value().decisionTick != tick)
                return std::nullopt;
            return Editor::EditorPlayAgentInspection{owner,
                                                     *handle,
                                                     tick,
                                                     std::make_shared<const AI::AiAgentInspectionSnapshot>(std::move(snapshot).Value()),
                                                     ReadAgentNavigation(owner),
                                                     committedDecisionTrace_ && decisionTraceOwner_ == owner &&
                                                             committedDecisionTrace_->Header().blackboard.agent == *handle
                                                         ? committedDecisionTrace_
                                                         : nullptr,
                                                     inspectionPolicy_};
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
        const auto settings = CaptureEditorDecisionPolicy(*policy.decisions);
        if (settings.HasError())
            return Result<void>::Failure(settings.ErrorValue());
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
        auto registered = AI::CaptureBuiltinQuerySchema();
        if (registered.HasError())
            return Result<void>::Failure(registered.ErrorValue());
        auto schema = std::make_shared<const AI::QuerySchemaRegistry>(std::move(registered).Value());
        CookedSceneQueryPlanCatalog queries;
        CookedSceneLocalPolicyCatalog models;
        const auto &localPolicy = settings.Value().localPolicy;
        const auto bindings =
            ConfigureSceneLocalPolicies(policy.decisions->controllers, localPolicy ? &*localPolicy : nullptr, policy.cooked->target);
        if (bindings.HasError())
            return Result<void>::Failure(bindings.ErrorValue());
        auto selected = AdmitCookedSceneControllers(policy.aiControllers, cooked.Value(),
                                                    {*resources_.assets_, environment, bindings.Value(), {}, schema}, limits.Value(),
                                                    cancellation, &queries, &models);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        policy.aiControllers = std::move(selected).Value();
        if (auto perception =
                PrepareDecisionPerception(policy, cooked.Value(), definition, providerSource, settings.Value().perceptionWorkUnitsLimit);
            perception.HasError())
            return perception;
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        const bool reactive = std::ranges::any_of(policy.aiControllers, [](const auto &controller) {
            return controller.perceptionEndpoints && !controller.perceptionEndpoints->decisionWatches.empty();
        });
        const auto &configured = settings.Value();
        const auto mode = reactive ? AI::AiSchedulingMode::BestEffort : configured.scheduler.mode;
        if ((reactive && configured.schedulerModeAuthored && configured.scheduler.mode != mode) ||
            (!reactive && queries.empty() && mode == AI::AiSchedulingMode::BestEffort))
            return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid,
                                                   "Editor Decision Play scheduler mode must match the authored Perception watches."));
        AI::AiSceneCookedDecisionExecution execution{*decisionJobs_, configured.scheduler, {}, {}};
        if (!models.empty()) {
            execution.localPolicies = std::make_shared<const CookedSceneLocalPolicyCatalog>(models);
            execution.agent.workUnitsPerTick = configured.scheduler.workUnitsPerTick;
        }
        execution.scheduler.mode = mode;
        if (reactive)
            execution.timing = AI::AiCookedDecisionTiming::BestEffortReactive;
        if (!queries.empty()) {
            auto driver = SceneDecisionQueries::Create(std::move(queries), std::move(schema), execution.scheduler,
                                                       configured.queryWorkUnitsLimit, configured.queryCacheLimits);
            if (driver.HasError())
                return Result<void>::Failure(driver.ErrorValue());
            decisionQueries_ = std::move(driver).Value();
            execution.queryTasks = decisionQueries_;
            if (!reactive && mode == AI::AiSchedulingMode::BestEffort)
                execution.timing = AI::AiCookedDecisionTiming::BestEffortQueries;
        }
        const auto inspection =
            Editor::EditorPlayAiPolicy{.scheduler = execution.scheduler,
                                       .perceptionLimit = configured.perceptionWorkUnitsLimit,
                                       .scenePerceptionWork = playPerception_
                                                                  ? std::optional<std::size_t>{playPerception_->execution.workUnitsPerTick}
                                                                  : std::nullopt,
                                       .queryLimit = configured.queryWorkUnitsLimit,
                                       .queryCache = decisionQueries_ && mode == AI::AiSchedulingMode::BestEffort
                                                         ? configured.queryCacheLimits
                                                         : std::nullopt};
        auto admitted = RegisterPresentationParticipants(policy, std::move(execution));
        if (admitted.HasValue()) {
            localPolicies_ = std::move(models);
            inspectionPolicy_ = inspection;
            inspectionKeys_ = std::move(policy.decisions->inspectionKeys);
            decisionDebugGranted_ = settings.Value().debugGranted;
            queryInspectionGranted_ = settings.Value().queryInspectionGranted;
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

    Result<void> ProjectPlaySceneHost::RunDecisionQueries(const Runtime::RuntimeSceneView &scene, const std::uint64_t tick) {
        Math::WorldCoordinate64 origin;
        if (mountedWorld_) {
            const auto lease = mountedWorld_->Origin().Lease();
            if (lease.HasError())
                return Result<void>::Failure(lease.ErrorValue());
            const auto frame = lease.Value().Get();
            if (frame.HasError())
                return Result<void>::Failure(frame.ErrorValue());
            origin = frame.Value().Origin();
        }
        if (decisionQueries_->UsesNavigation())
            CommitQueryNavigation(tick);
        const SceneQueryNavigationSource navigation{this, [](const void *owner, const Runtime::RuntimeSceneView &current,
                                                             Runtime::EntityRef querier, std::uint64_t now) noexcept {
            return static_cast<const ProjectPlaySceneHost *>(owner)->ReadQueryNavigation(current, querier, now);
        }};
        const SceneQueryDecisionPhase evaluate{this, [](void *owner, AI::AiTaskScheduler &scheduler, std::uint64_t now) {
            return static_cast<ProjectPlaySceneHost *>(owner)->RunDecisionScheduler(scheduler, now);
        }};
        return decisionQueries_->RunAtFixedTick(*ai_, scene, origin, tick, &native_, navigation, evaluate);
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
        const auto scene = scenes_->ActiveScene();
        if (!scene)
            return Evaluation::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        const auto decision = decisionQueries_ ? RunDecisionQueries(*scene, tick) : RunDecisionScheduler(*scheduler.Value(), tick);
        if (decision.HasError())
            return Evaluation::Failure(decision.ErrorValue());
        decisionTickPending_ = true;
        return Evaluation::Success(true);
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
