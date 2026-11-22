#include "AISceneRuntimeDetail.h"
#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionQueryTask.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Keeps nested provider callbacks from replacing the decision being prepared or retired. */
        class DecisionReplacementGuard final {
        public:
            explicit DecisionReplacementGuard(bool &replacing) noexcept : replacing_(replacing) {
                replacing = true;
            }

            ~DecisionReplacementGuard() {
                replacing_ = false;
            }

            DecisionReplacementGuard(const DecisionReplacementGuard &) = delete;
            DecisionReplacementGuard &operator=(const DecisionReplacementGuard &) = delete;

        private:
            bool &replacing_;
        };

        /** @brief Resolves only an active exact agent after the caller has checked the real owner lane. */
        Result<Detail::AgentRuntimeState *> DecisionAgent(Detail::AiSceneRuntimeState &state, const AgentHandle agent) {
            auto *slot = Detail::FindAgent(state, agent);
            if (!slot || slot->record.state != AiAgentActivationState::Active || !slot->blackboard || !slot->blackboard->IsActive())
                return Result<Detail::AgentRuntimeState *>::Failure(MakeError(AIErrors::HandleInvalid));
            return Result<Detail::AgentRuntimeState *>::Success(slot);
        }

        /** @brief Freezes the blackboard owned by an exact active agent generation. */
        Result<BlackboardSnapshot> CaptureDecisionSnapshot(Detail::AiSceneRuntimeState &state, const AgentHandle agent) {
            const auto selected = DecisionAgent(state, agent);
            if (selected.HasError())
                return Result<BlackboardSnapshot>::Failure(selected.ErrorValue());
            return selected.Value()->blackboard->Snapshot();
        }

        Result<Detail::AgentRuntimeState> PrepareDecisionSchemaMigration(const Detail::AgentRuntimeState &agent,
                                                                         std::shared_ptr<const BlackboardSchema> schema,
                                                                         const BlackboardSchemaMigration &migration) {
            using Staged = Result<Detail::AgentRuntimeState>;
            auto binding = agent.blackboardBinding;
            if (migration.source != agent.schema || schema->Identity() != binding.schema || schema->Version() <= binding.schemaVersion)
                return Staged::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            if (agent.task || agent.sensing)
                return Staged::Failure(MakeError(AIErrors::DecisionAssetSchemaIncompatible));
            if (binding.schemaGeneration == std::numeric_limits<std::uint64_t>::max())
                return Staged::Failure(MakeError(AIErrors::BlackboardRevisionExhausted));
            const auto instanceGeneration = AdvanceAiRuntimeGeneration(binding.instanceGeneration);
            if (instanceGeneration.HasError())
                return Staged::Failure(instanceGeneration.ErrorValue());
            binding.schemaVersion = schema->Version();
            ++binding.schemaGeneration;
            binding.instanceGeneration = instanceGeneration.Value();

            auto source = agent.blackboard->CaptureCanonical();
            if (source.HasError())
                return Staged::Failure(source.ErrorValue());
            auto migrated = MigrateCanonicalBlackboard(source.Value(), *schema, std::span{&migration, 1});
            if (migrated.HasError())
                return Staged::Failure(migrated.ErrorValue());
            auto board = BlackboardInstance::CreateFromCanonical(binding, schema, migrated.Value());
            if (board.HasError())
                return Staged::Failure(board.ErrorValue());
            try {
                return Staged::Success({.record = agent.record,
                                        .schema = std::move(schema),
                                        .perceptionEndpoints = agent.perceptionEndpoints,
                                        .blackboardBinding = binding,
                                        .blackboard = std::move(board).Value()});
            } catch (const std::bad_alloc &) {
                return Staged::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        std::size_t ModelWork(const AiSceneLocalPolicyBinding &policy) noexcept {
            return policy.model->RequiredWork() + policy.featureKeys.size() * std::bit_width(MaximumBlackboardKeys);
        }

        bool CaptureModelFeatures(const AiSceneLocalPolicyBinding &policy, const BlackboardSnapshot &snapshot,
                                  const std::span<LocalPolicyFeature> output) {
            const auto ids = policy.model->Features();
            for (std::size_t index = 0; index < output.size(); ++index) {
                const auto value = snapshot.Read(policy.featureKeys[index]);
                if (value.HasError() || !value.Value())
                    return false;
                const auto *scalar = std::get_if<BlackboardScalarValue>(&*value.Value());
                const auto *integer = scalar ? std::get_if<std::int64_t>(scalar) : nullptr;
                if (!integer || !std::in_range<std::int16_t>(*integer))
                    return false;
                output[index] = {ids[index], static_cast<std::int16_t>(*integer)};
            }
            return true;
        }

        Result<std::shared_ptr<const AiSceneLocalPolicyBinding>> BindModel(const AiSceneCookedDecisionExecution &execution,
                                                                           const Detail::AgentRuntimeState &agent,
                                                                           const DecisionCompiledGraph &root) {
            using Bound = Result<std::shared_ptr<const AiSceneLocalPolicyBinding>>;
            if (!execution.localPolicies || !agent.record.controller)
                return Bound::Success({});
            const auto entry = execution.localPolicies->find(agent.record.controller->controller);
            if (entry == execution.localPolicies->end())
                return Bound::Success({});
            const auto &policy = entry->second;
            if (!policy || !policy->model)
                return Bound::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
            if (policy->featureKeys.empty())
                return Bound::Success({});
            if (!std::holds_alternative<std::shared_ptr<const CookedUtilityPlan>>(root.execution) || !agent.schema ||
                !policy->model->SupportsBlackboard(agent.schema->Identity()) ||
                policy->featureKeys.size() != policy->model->Features().size())
                return Bound::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            const auto keys = agent.schema->Keys();
            for (const auto id : policy->featureKeys) {
                const auto key = std::ranges::lower_bound(keys, id, {}, &BlackboardKeyDescriptor::key);
                if (key == keys.end() || key->key != id || key->kind != BlackboardValueKind::SignedInteger ||
                    key->cardinality != BlackboardValueCardinality::Scalar)
                    return Bound::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            }
            return Bound::Success(policy);
        }

        /** @brief Flat cooked decisions own their task lease and borrow only a weak exact Scene publication. */
        class ScheduledCookedDecision final : public IAiScheduledDecision {
        public:
            ScheduledCookedDecision(const std::shared_ptr<Detail::AiSceneRuntimeState> &state,
                                    std::unique_ptr<DecisionGraphInstance> decision, AgentHandle agent,
                                    std::shared_ptr<IAiCookedDecisionIntentSink> intents, std::vector<PerceptionListenerTypeId> watches,
                                    bool reactive, std::shared_ptr<const AiSceneLocalPolicyBinding> model,
                                    const std::size_t graphWork) noexcept
                : state_(state), decision_(std::move(decision)), agent_(agent), intents_(std::move(intents)), watches_(std::move(watches)),
                  reactive_(reactive), model_(std::move(model)), graphWork_(graphWork) {}

            Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                providerFailure_.reset();
                evaluationTick_ = context.tick;
                const auto state = state_.lock();
                if (!state || state->closed || state->owner != std::this_thread::get_id())
                    return Result<AiSliceOutcome>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                const auto snapshot = CaptureDecisionSnapshot(*state, context.agent);
                if (snapshot.HasError())
                    return Result<AiSliceOutcome>::Failure(snapshot.ErrorValue());
                const auto &reasons = context.reasons;
                const auto resume =
                    reasons.blackboard || reasons.perception || reasons.task ? AiTaskResumeReason::Event : AiTaskResumeReason::FixedTick;
                const DecisionGraphStepInput input{.tick = context.tick,
                                                   .current = snapshot.Value().Binding(),
                                                   .blackboard = snapshot.Value(),
                                                   .events = {},
                                                   .treeResumeReason = resume};
                auto &capture = state->decisionReplay;
                const auto trace = capture && capture->agent == context.agent && capture->tick == context.tick &&
                                           capture->executor == this && !capture->observed
                                       ? capture->trace
                                       : nullptr;
                const auto step = model_ ? StepModel(input, budget, trace.get()) : StepGraph(input, budget, trace.get());
                if (trace && capture && capture->trace == trace) {
                    const auto revision = input.blackboard.CapturedRevision();
                    if (trace->Header().MatchesObservation(capture->plan->Digest(), context.tick, input.current, revision))
                        capture->observed = AiReplayDecisionObservation{capture->subject, input.current, revision, trace.get()};
                }
                if (step.HasError())
                    return Result<AiSliceOutcome>::Failure(step.ErrorValue());
                if (reactive_ && step.Value())
                    nextFixedWake_ = decision_->NextFixedWakeTick();
                return Result<AiSliceOutcome>::Success(step.Value() ? AiSliceOutcome::Complete : AiSliceOutcome::BudgetExhausted);
            }

            std::optional<DecisionProviderFailure> TakeProviderFailure(std::uint64_t tick) noexcept override {
                return tick == evaluationTick_ ? std::exchange(providerFailure_, std::nullopt) : std::nullopt;
            }

            Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                return intents_ ? intents_->Commit(context, budget) : Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                if (intents_)
                    intents_->Discard(agent_);
                providerFailure_.reset();
                nextFixedWake_.reset();
                static_cast<void>(decision_->Cancel(AiTaskCancellationReason::AgentGenerationRetired));
            }

            void CancelForReplacement() noexcept override {
                if (intents_)
                    intents_->Discard(agent_);
                providerFailure_.reset();
                nextFixedWake_.reset();
                static_cast<void>(decision_->Cancel(AiTaskCancellationReason::PlanReplaced));
            }

            bool HasPendingTaskWake() const noexcept override {
                return decision_->HasPendingTaskWake();
            }

            std::optional<std::uint64_t> NextFixedWakeTick() const noexcept override {
                return nextFixedWake_;
            }

            std::span<const PerceptionListenerTypeId> DeclaredPerceptionWatches() const noexcept override {
                return watches_;
            }

            DecisionGraphInstance &Decision() noexcept {
                return *decision_;
            }

            const DecisionGraphInstance &Decision() const noexcept {
                return *decision_;
            }

            bool HasModel() const noexcept {
                return model_ != nullptr;
            }

        private:
            Result<std::optional<AiTaskState>> StepGraph(const DecisionGraphStepInput &input, AiWorkBudget &budget,
                                                         DecisionStepTrace *trace) {
                return trace ? decision_->StepBudgetedWithTrace(input, budget, *trace, &providerFailure_)
                             : decision_->StepBudgeted(input, budget, &providerFailure_);
            }

            Result<std::optional<AiTaskState>> StepModel(const DecisionGraphStepInput &input, AiWorkBudget &budget,
                                                         DecisionStepTrace *trace) {
                const auto modelWork = ModelWork(*model_);
                const auto total = graphWork_ + modelWork;
                if (!budget.Consume(total))
                    return Result<std::optional<AiTaskState>>::Success(std::nullopt);
                AiWorkBudget graphBudget{graphWork_};
                const auto &model = *model_->model;
                std::array<LocalPolicyFeature, LocalLearnedPolicy::MaximumFeatures> features{};
                const auto output = std::span{features}.first(model_->featureKeys.size());
                if (!CaptureModelFeatures(*model_, input.blackboard, output))
                    return StepGraph(input, graphBudget, trace);
                AiWorkBudget inferenceBudget{model.RequiredWork()};
                const LocalPolicyUtilityInput policy{model,
                                                     model_->tasks,
                                                     output,
                                                     model.Digest(),
                                                     model.Target(),
                                                     model.TensorSchemaVersion(),
                                                     input.blackboard.CapturedRevision(),
                                                     inferenceBudget};
                return decision_->StepBudgetedWithLocalPolicy(input, graphBudget, policy, trace);
            }

            std::weak_ptr<Detail::AiSceneRuntimeState> state_;
            std::unique_ptr<DecisionGraphInstance> decision_;
            AgentHandle agent_;
            std::shared_ptr<IAiCookedDecisionIntentSink> intents_;
            std::vector<PerceptionListenerTypeId> watches_;
            std::optional<DecisionProviderFailure> providerFailure_;
            std::uint64_t evaluationTick_{};
            std::optional<std::uint64_t> nextFixedWake_;
            bool reactive_{};
            std::shared_ptr<const AiSceneLocalPolicyBinding> model_;
            std::size_t graphWork_{};
        };
    }  // namespace

    /** @copydoc AiSceneRuntime::CopyCookedDecisionTasks */
    std::size_t AiSceneRuntime::CopyCookedDecisionTasks(const AgentHandle agent, const std::shared_ptr<const CookedDecisionPlan> &plan,
                                                        const std::span<DecisionActiveTask> output) const {
        const auto *executor = dynamic_cast<const ScheduledCookedDecision *>(active_->scheduler->InspectDecision(agent, plan));
        return executor ? executor->Decision().CopyActiveTasks(output) : 0;
    }

    /** @copydoc AiSceneRuntime::ArmDecisionReplayAtSafePoint */
    Result<void> AiSceneRuntime::ArmDecisionReplayAtSafePoint(const AgentHandle agent, const std::uint64_t tick) const {
        return ArmCookedDecisionEvidence(agent, tick, true);
    }

    /** @copydoc AiSceneRuntime::ArmDecisionTraceAtSafePoint */
    Result<void> AiSceneRuntime::ArmDecisionTraceAtSafePoint(const AgentHandle agent, const std::uint64_t tick) const {
        return ArmCookedDecisionEvidence(agent, tick, false);
    }

    Result<void> AiSceneRuntime::ArmCookedDecisionEvidence(const AgentHandle agent, const std::uint64_t tick, const bool replay) const {
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return valid;
        auto *scheduler = active_->scheduler.get();
        if (!scheduler || !decisionExecution_ || decisionExecution_->timing != AiCookedDecisionTiming::Deterministic)
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        const auto previousTick = scheduler->report_.tick;
        if (tick == 0 || (previousTick != 0 && (tick <= previousTick || tick - previousTick != 1)))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        const auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        const auto &slot = *selected.Value();
        const auto registered = scheduler->ValidateDecisionReplacement(agent, slot.cookedPlan);
        if (registered.HasError())
            return Result<void>::Failure(registered.ErrorValue());
        const auto *executor = dynamic_cast<const ScheduledCookedDecision *>(registered.Value());
        if (!executor || (replay && executor->HasModel()) || !slot.cookedPlan->IsCurrent())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        try {
            auto trace = std::make_shared<DecisionStepTrace>();
            active_->decisionReplay.emplace(Detail::SceneDecisionReplayPending{.agent = agent,
                                                                               .subject = slot.record.agent.agent,
                                                                               .tick = tick,
                                                                               .schedulerRevision = scheduler->populationRevision_,
                                                                               .plan = slot.cookedPlan,
                                                                               .executor = executor,
                                                                               .trace = std::move(trace),
                                                                               .replay = replay});
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::DiscardDecisionReplayAtSafePoint */
    void AiSceneRuntime::DiscardDecisionReplayAtSafePoint(const AgentHandle agent, const std::uint64_t tick) const noexcept {
        if (!active_ || std::this_thread::get_id() != active_->owner || active_->closed)
            return;
        auto &pending = active_->decisionReplay;
        if (pending && pending->agent == agent && pending->tick == tick)
            pending.reset();
    }

    /** @copydoc AiSceneRuntime::FindDecisionTraceAtSafePoint */
    Result<Detail::SceneDecisionReplayPending *> AiSceneRuntime::FindDecisionTraceAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                              const std::uint64_t tick) const {
        using Pending = Result<Detail::SceneDecisionReplayPending *>;
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return Pending::Failure(valid.ErrorValue());
        const auto state = active_;
        auto *scheduler = state->scheduler.get();
        const auto *report = scheduler ? scheduler->CompletedReportAtSafePoint() : nullptr;
        if (!report || report->tick != tick)
            return Pending::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (!scene.IsCurrent() || scene.RuntimeId() != state->binding.scene || !state->decisionReplay ||
            state->decisionReplay->tick != tick || !state->decisionReplay->observed ||
            state->decisionReplay->schedulerRevision != scheduler->populationRevision_)
            return Pending::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        auto &pending = *state->decisionReplay;
        const auto &observed = *pending.observed;
        const auto selected = DecisionAgent(*state, pending.agent);
        if (selected.HasError() || selected.Value()->cookedPlan != pending.plan ||
            selected.Value()->blackboard->Binding() != observed.blackboard)
            return Pending::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        return Pending::Success(&pending);
    }

    /** @copydoc AiSceneRuntime::TakeDecisionTraceAtSafePoint */
    Result<std::shared_ptr<const DecisionStepTrace>> AiSceneRuntime::TakeDecisionTraceAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                                  const AgentHandle agent,
                                                                                                  const std::uint64_t tick) const {
        using Trace = Result<std::shared_ptr<const DecisionStepTrace>>;
        const auto found = FindDecisionTraceAtSafePoint(scene, tick);
        if (found.HasError())
            return Trace::Failure(found.ErrorValue());
        if (found.Value()->agent != agent)
            return Trace::Failure(MakeError(AIErrors::HandleInvalid));
        auto trace = std::move(found.Value()->trace);
        active_->decisionReplay.reset();
        return Trace::Success(std::move(trace));
    }

    /** @copydoc AiSceneRuntime::CaptureDecisionReplayAtSafePoint */
    Result<AiSceneDecisionReplayCapture> AiSceneRuntime::CaptureDecisionReplayAtSafePoint(const Runtime::RuntimeSceneView scene,
                                                                                          const std::uint64_t tick,
                                                                                          const std::size_t maximumCanonicalBytes) const {
        using Capture = Result<AiSceneDecisionReplayCapture>;
        const auto found = FindDecisionTraceAtSafePoint(scene, tick);
        if (found.HasError())
            return Capture::Failure(found.ErrorValue());
        auto &pending = *found.Value();
        if (!pending.replay || pending.trace->DroppedRecords() != 0)
            return Capture::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        const auto &observed = *pending.observed;
        auto canonical = CaptureCanonicalState(scene, maximumCanonicalBytes);
        if (canonical.HasError())
            return Capture::Failure(canonical.ErrorValue());
        AiSceneDecisionReplayCapture capture{scene.RuntimeId(),        scene.StructuralRevision(),  tick,
                                             observed.subject,         observed.blackboard,         observed.blackboardRevision,
                                             std::move(pending.trace), std::move(canonical).Value()};
        active_->decisionReplay.reset();
        return Capture::Success(std::move(capture));
    }

    /** @copydoc AiSceneRuntime::ResolveDecisionQueryOwnerAtSafePoint */
    Result<Runtime::EntityRef> AiSceneRuntime::ResolveDecisionQueryOwnerAtSafePoint(AgentHandle agent) const {
        using Owner = Result<Runtime::EntityRef>;
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return Owner::Failure(valid.ErrorValue());
        const auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Owner::Failure(selected.ErrorValue());
        return Owner::Success(selected.Value()->record.owner);
    }

    /** @copydoc AiSceneRuntime::ReadDecisionFailureAtSafePoint */
    Result<std::optional<AiRuntimeDecisionDiagnostic>> AiSceneRuntime::ReadDecisionFailureAtSafePoint(
        const Runtime::RuntimeSceneView &scene) const {
        using Diagnostic = Result<std::optional<AiRuntimeDecisionDiagnostic>>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Diagnostic::Failure(owner.ErrorValue());
        const auto scheduler = TaskSchedulerAtSafePoint();
        if (scheduler.HasError())
            return Diagnostic::Failure(scheduler.ErrorValue());
        if (!scene.IsCurrent() || scene.RuntimeId() != active_->binding.scene)
            return Diagnostic::Failure(MakeError(AIErrors::TaskContextInvalid, "Decision diagnostics require the current AI Scene."));
        const auto *report = scheduler.Value()->CompletedReportAtSafePoint();
        if (!report)
            return Diagnostic::Success(std::nullopt);
        const bool fatal = report->firstFailure && report->firstFailureOrigin;
        const auto *advisory = report->firstAdvisoryFailure ? &*report->firstAdvisoryFailure : nullptr;
        if (!fatal && !advisory)
            return Diagnostic::Success(std::nullopt);
        const auto &origin = fatal ? *report->firstFailureOrigin : advisory->origin;
        const auto &error = fatal ? *report->firstFailure : advisory->error;
        const auto *slot = Detail::FindAgent(*active_, origin.agent);
        std::optional<Runtime::EntityRef> entity;
        if (slot && slot->record.state == AiAgentActivationState::Active && slot->record.agent.agent == origin.identity &&
            scene.Get(slot->record.owner).HasValue())
            entity = slot->record.owner;
        return Diagnostic::Success(AiRuntimeDecisionDiagnostic{report->tick, origin, error, entity});
    }

    /** @copydoc AiSceneRuntime::SyncDecisionQueryAtBlackboardSync */
    Result<bool> AiSceneRuntime::SyncDecisionQueryAtBlackboardSync(DecisionQueryTask &task, const Runtime::RuntimeSceneView &scene,
                                                                   std::uint64_t tick,
                                                                   std::optional<DecisionQueryPublication> *publication) const {
        using Sync = Result<bool>;
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return Sync::Failure(valid.ErrorValue());
        if (tick == 0)
            return Sync::Failure(MakeError(AIErrors::TaskContextInvalid));
        const auto state = active_;
        if (!scene.IsCurrent() || scene.RuntimeId() != state->binding.scene)
            return Sync::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto owner = task.ActiveAgent();
        if (!owner.IsValid())
            return Sync::Success(false);
        if (const auto before = DecisionAgent(*state, owner); before.HasError())
            return Sync::Failure(before.ErrorValue());
        task.SyncQuery(tick, publication);
        if (task.ActiveAgent() != owner)
            return Sync::Success(false);
        auto ready = task.PrepareOutput(tick, scene);
        if (ready.HasError() || !ready.Value())
            return ready;
        if (active_ != state || !scene.IsCurrent() || task.ActiveAgent() != owner)
            return Sync::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto selected = DecisionAgent(*state, owner);
        if (selected.HasError())
            return Sync::Failure(selected.ErrorValue());
        return task.CommitPreparedOutput(scene, *selected.Value()->blackboard, publication);
    }

    /** @copydoc AiSceneRuntime::PrepareCookedDecisionRegistration */
    Result<AiScheduledDecisionRegistration> AiSceneRuntime::PrepareCookedDecisionRegistration(
        const std::shared_ptr<Detail::AiSceneRuntimeState> &state, const Detail::AgentRuntimeState &agent,
        const std::shared_ptr<const CookedDecisionPlan> &plan, std::uint32_t &nextTaskSlot) const {
        using Output = Result<AiScheduledDecisionRegistration>;
        auto bounded = decisionExecution_->agent;
        const auto limit = plan->MaximumDecisionWorkUnits();
        if (!limit)
            return Output::Failure(MakeError(AIErrors::CapabilityUnavailable, "Scene scheduler requires a flat cooked decision."));
        const auto &root = plan->Graphs()[plan->Occurrences().front().plan];
        auto model = BindModel(*decisionExecution_, agent, root);
        if (model.HasError())
            return Output::Failure(std::move(model).ErrorValue());
        const auto required = *limit + (model.Value() ? ModelWork(*model.Value()) : 0);
        if (required > bounded.workUnitsPerTick)
            return Output::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        if (!root.executionTask.IsValid())
            return Output::Failure(
                MakeError(AIErrors::TaskContextInvalid, "Scheduled cooked decisions require an authored execution task."));

        std::vector<PerceptionListenerTypeId> watches;
        if (agent.perceptionEndpoints && !agent.perceptionEndpoints->decisionWatches.empty()) {
            if (decisionExecution_->scheduler.mode != AiSchedulingMode::BestEffort)
                return Output::Failure(
                    MakeError(AIErrors::SchedulerPolicyInvalid, "Declared Perception wakes require best-effort timing."));
            auto admitted =
                Detail::ResolveCookedPerceptionWatches(*plan, *agent.perceptionEndpoints, agent.perceptionEndpoints->decisionWatches);
            if (admitted.HasError())
                return Output::Failure(admitted.ErrorValue());
            watches = std::move(admitted).Value();
            bounded.intervalTicks = 0;
        }

        constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
        const auto taskSlots = plan->Storage().taskSlots;
        if (taskSlots >= End - nextTaskSlot)
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        const auto count = static_cast<std::uint32_t>(taskSlots);
        const auto handle = agent.record.handle;
        const auto identity = agent.record.agent.agent;
        const auto cancellation = agent.cancellation.Token();
        const AiTaskOperationContext operation{.taskDefinition = root.executionTask,
                                               .task = {handle.incarnation, {nextTaskSlot, 1}},
                                               .agent = handle,
                                               .cancellation = cancellation};
        try {
            AiDecisionTaskReservation reservation{agent.blackboardBinding, agent.schema, nextTaskSlot + 1, count, cancellation};
            auto decision = DecisionGraphInstance::Create(plan, std::move(reservation), operation, decisionExecution_->queryTasks);
            if (decision.HasError())
                return Output::Failure(decision.ErrorValue());
            bounded.workUnitsPerTick = required;
            auto executor =
                std::make_shared<ScheduledCookedDecision>(state, std::move(decision).Value(), handle, decisionExecution_->intents,
                                                          std::move(watches), bounded.intervalTicks == 0, std::move(model).Value(), *limit);
            nextTaskSlot += count + 1;
            return Output::Success({handle, identity, bounded, plan, std::move(executor), cancellation});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::PrepareCookedDecisionRegistrations */
    Result<std::vector<AiScheduledDecisionRegistration>> AiSceneRuntime::PrepareCookedDecisionRegistrations(
        const std::shared_ptr<Detail::AiSceneRuntimeState> &state, const std::span<const Detail::AgentRuntimeState> agents,
        std::uint32_t &nextTaskSlot) const {
        using Output = Result<std::vector<AiScheduledDecisionRegistration>>;
        if (!decisionExecution_)
            return Output::Success({});
        auto cursor = nextTaskSlot;
        try {
            std::vector<AiScheduledDecisionRegistration> registrations;
            for (const auto &agent : agents) {
                const auto &plan = agent.cookedPlan;
                if (!plan)
                    continue;
                auto registration = PrepareCookedDecisionRegistration(state, agent, plan, cursor);
                if (registration.HasError())
                    return Output::Failure(registration.ErrorValue());
                registrations.push_back(std::move(registration).Value());
            }
            nextTaskSlot = cursor;
            return Output::Success(std::move(registrations));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::ReplaceCookedDecisionAtSafePoint */
    Result<void> AiSceneRuntime::ReplaceCookedDecisionAtSafePoint(const AgentHandle agent,
                                                                  const std::shared_ptr<const CookedDecisionPlan> previous,
                                                                  std::shared_ptr<const CookedDecisionPlan> replacement,
                                                                  const BlackboardSchemaMigration *migration) {
        using Outcome = Result<void>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return owner;
        const auto state = active_;
        auto *scheduler = state->scheduler.get();
        if (state->replacingDecision)
            return Outcome::Failure(MakeError(AIErrors::SchedulerPhaseInvalid, "A cooked decision replacement is already in progress."));
        if (!decisionExecution_ || !scheduler)
            return Outcome::Failure(MakeError(AIErrors::RuntimeUnavailable));
        const auto selected = DecisionAgent(*state, agent);
        if (selected.HasError())
            return Outcome::Failure(selected.ErrorValue());
        auto &slot = *selected.Value();
        if (!previous || slot.cookedPlan != previous || !replacement || replacement == previous ||
            replacement->Root() != previous->Root() || !replacement->IsCurrent() ||
            state->revision == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        if ((replacement->Capabilities().bits & ~previous->Capabilities().bits) != 0)
            return Outcome::Failure(MakeError(AIErrors::CapabilityUnavailable));
        const auto registered = scheduler->ValidateDecisionReplacement(agent, previous);
        if (registered.HasError())
            return Outcome::Failure(registered.ErrorValue());
        auto *executor = dynamic_cast<ScheduledCookedDecision *>(registered.Value());
        if (!executor)
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        const auto &oldRoot = previous->Graphs()[previous->Occurrences().front().plan];
        const auto &newRoot = replacement->Graphs()[replacement->Occurrences().front().plan];
        if (oldRoot.execution.index() != newRoot.execution.index())
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        const DecisionReplacementGuard replacing{state->replacingDecision};
        const auto schema = newRoot.bindings->BlackboardSchema();
        if (schema != slot.schema) {
            if (!migration || !schema)
                return Outcome::Failure(MakeError(AIErrors::DecisionAssetSchemaIncompatible));
            auto prepared = PrepareDecisionSchemaMigration(slot, schema, *migration);
            if (prepared.HasError())
                return Outcome::Failure(prepared.ErrorValue());
            auto migrated = std::move(prepared).Value();
            return RestartCookedDecisionAtSafePoint(state, slot, std::move(replacement), &migrated);
        }
        auto retained = executor->Decision().TryRetainCompatibleFlatPlan(replacement);
        if (retained.HasError())
            return Outcome::Failure(retained.ErrorValue());
        if (!retained.Value())
            return RestartCookedDecisionAtSafePoint(state, slot, std::move(replacement), nullptr);
        scheduler->PublishRetainedDecisionImage(agent, replacement);
        slot.cookedPlan = std::move(replacement);
        ++state->revision;
        return Outcome::Success();
    }

    /** @copydoc AiSceneRuntime::RestartCookedDecisionAtSafePoint */
    Result<void> AiSceneRuntime::RestartCookedDecisionAtSafePoint(const std::shared_ptr<Detail::AiSceneRuntimeState> &state,
                                                                  Detail::AgentRuntimeState &slot,
                                                                  std::shared_ptr<const CookedDecisionPlan> replacement,
                                                                  Detail::AgentRuntimeState *migrated) {
        using Outcome = Result<void>;
        const auto agent = slot.record.handle;
        const auto previous = slot.cookedPlan;
        auto *scheduler = state->scheduler.get();
        const auto sceneRevision = state->revision;
        const auto scheduleRevision = scheduler->populationRevision_;
        const auto tick = scheduler->report_.tick;
        auto cursor = state->nextTaskSlot;
        auto registration = PrepareCookedDecisionRegistration(state, migrated ? *migrated : slot, replacement, cursor);
        if (registration.HasError())
            return Outcome::Failure(registration.ErrorValue());
        // Provider construction can call out. Reacquire the slot only after validating the original publication.
        if (active_ != state || state->closed || state->revision != sceneRevision || state->scheduler.get() != scheduler ||
            scheduler->populationRevision_ != scheduleRevision || scheduler->report_.tick != tick || !replacement->IsCurrent())
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        const auto current = DecisionAgent(*state, agent);
        if (current.HasError() || current.Value()->cookedPlan != previous)
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        auto staged = scheduler->PrepareDecisionReplacement(registration.Value(), previous);
        if (staged.HasError())
            return Outcome::Failure(staged.ErrorValue());
        if (auto valid = staged.Value()->ValidatePublication(); valid.HasError())
            return valid;
        if (!replacement->IsCurrent())
            return Outcome::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        if (migrated) {
            auto &live = *current.Value();
            if (auto revoked = live.blackboard->TeardownAtBlackboardSync(); revoked.HasError())
                return revoked;
            live.cancellation.RequestCancellation();
            live.cancellation = std::move(migrated->cancellation);
            live.schema = std::move(migrated->schema);
            live.blackboardBinding = migrated->blackboardBinding;
            live.blackboard = std::move(migrated->blackboard);
        }
        // No provider callbacks intervene between publication and the cursor update.
        staged.Value()->Publish();
        current.Value()->cookedPlan = std::move(replacement);
        state->nextTaskSlot = cursor;
        ++state->revision;
        staged.Value()->FinishRetirements();
        return Outcome::Success();
    }

    /** @copydoc AiSceneRuntime::PrepareCookedDecisionScheduler */
    Result<void> AiSceneRuntime::PrepareCookedDecisionScheduler(const std::shared_ptr<Detail::AiSceneRuntimeState> &state) const {
        if (!decisionExecution_)
            return Result<void>::Success();
        const auto revision = state->revision;
        auto scheduler = AiTaskScheduler::Create(decisionExecution_->jobs, state->binding.incarnation, decisionExecution_->scheduler);
        if (scheduler.HasError())
            return Result<void>::Failure(scheduler.ErrorValue());
        auto cursor = state->nextTaskSlot;
        auto registrations = PrepareCookedDecisionRegistrations(state, state->agents, cursor);
        if (registrations.HasError())
            return Result<void>::Failure(registrations.ErrorValue());
        auto staged = scheduler.Value()->PreparePopulationChange(registrations.Value(), {});
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        if (state->closed || state->revision != revision || state->scheduler)
            return Result<void>::Failure(
                MakeError(AIErrors::SceneActivationInvalid, "The Scene owner changed while cooked decisions were prepared."));
        if (auto valid = staged.Value()->ValidatePublication(); valid.HasError())
            return valid;
        staged.Value()->Publish();
        state->scheduler = std::move(scheduler).Value();
        state->nextTaskSlot = cursor;
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::ActivateCookedDecisionsAtSafePoint */
    Result<void> AiSceneRuntime::ActivateCookedDecisionsAtSafePoint(AiSceneCookedDecisionExecution execution) {
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return valid;
        const auto state = active_;
        if (decisionExecution_ || state->scheduler || !execution.HasMatchingTiming())
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        decisionExecution_.emplace(std::move(execution));
        auto prepared = PrepareCookedDecisionScheduler(state);
        if (prepared.HasError()) {
            decisionExecution_.reset();
            return prepared;
        }
        ++state->revision;
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint */
    Result<AiDecisionTaskReservation> AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint(const AgentHandle agent,
                                                                                          const std::uint32_t count) const {
        using Output = Result<AiDecisionTaskReservation>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        if (active_->revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
        if (count > End - active_->nextTaskSlot)
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        try {
            auto &slot = *selected.Value();
            AiDecisionTaskReservation reservation{slot.blackboard->Binding(), slot.blackboard->SchemaAtSafePoint(), active_->nextTaskSlot,
                                                  count, slot.cancellation.Token()};
            active_->nextTaskSlot += count;
            ++active_->revision;
            return Output::Success(std::move(reservation));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint */
    Result<BlackboardSnapshot> AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint(const AgentHandle agent) const {
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Result<BlackboardSnapshot>::Failure(owner.ErrorValue());
        return CaptureDecisionSnapshot(*active_, agent);
    }
}  // namespace Horo::AI
