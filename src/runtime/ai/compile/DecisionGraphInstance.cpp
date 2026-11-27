#include "DecisionInstanceState.h"

#include <algorithm>
#include <limits>
#include <new>
#include <tuple>

namespace Horo::AI {
    /** @copydoc DecisionGraphInstance::CopyActiveTasks */
    std::size_t DecisionGraphInstance::CopyActiveTasks(const std::span<DecisionActiveTask> output) const noexcept {
        if (owner_ != std::this_thread::get_id() || evaluating_ || shutdown_)
            return 0;
        std::size_t count{};
        for (std::size_t index = 0; index < occurrences_.size(); ++index) {
            const auto &state = *occurrences_[index];
            if (!state.active)
                continue;
            const auto occurrence = TraceOccurrence(index);
            const auto treeTasks = [&](const BehaviorTreeInstance &tree) {
                count += tree.CopyActiveTasks(occurrence, output.subspan(std::min(count, output.size())));
            };
            if (state.tree)
                treeTasks(*state.tree);
            if (state.utility && state.utility->selected)
                treeTasks(*state.utility->actions[*state.utility->selected]);
            if (!state.actions)
                continue;
            for (const auto &action : state.actions->actions) {
                if (action->lifecycle.State() != AiTaskState::Running || !action->lifecycle.Context())
                    continue;
                const auto &context = *action->lifecycle.Context();
                if (count < output.size())
                    output[count] = {occurrence, action->node, context.taskDefinition, context.task};
                ++count;
            }
        }
        return count;
    }

    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::TaskContextInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        bool Terminal(AiTaskState state) noexcept {
            using enum Horo::AI::AiTaskState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        /** @brief Retains the query host with the exact executable generation without changing the cooked instance layout. */
        struct QueryProviderLease final {
            std::shared_ptr<const DecisionNodeGeneration> providers;
            std::shared_ptr<IDecisionQueryTaskBinder> queryTasks;
        };

        bool ValidRange(const DecisionGraphInstanceBinding &binding, const CookedDecisionPlan &plan) noexcept {
            const auto count = plan.Storage().taskSlots;
            if (binding.firstTaskGeneration == 0 || count > binding.availableTaskSlots || count > std::numeric_limits<std::uint32_t>::max())
                return false;
            return binding.firstTaskSlot <= std::numeric_limits<std::uint32_t>::max() - count;
        }

        Result<void> AdmitBinding(const DecisionGraphInstanceBinding &binding, const CookedDecisionPlan &plan) {
            if (!binding.blackboard.IsValid() || binding.operation.agent != binding.blackboard.agent || !ValidRange(binding, plan))
                return Invalid<void>();
            AiTaskLifecycle admission;
            if (auto admitted = admission.Start(binding.operation); admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            return admission.State() == AiTaskState::Running ? Result<void>::Success() : Invalid<void>();
        }

        bool MatchesBlackboard(const DecisionGraphStepInput &input, const BlackboardInstanceBinding &binding,
                               const std::shared_ptr<const BlackboardSchema> &schema) noexcept {
            return input.current == binding && input.blackboard.Binding() == input.current && schema && input.blackboard.Schema() == schema;
        }
    }  // namespace

    /** @brief Scopes the graph's borrowed evaluation inputs and evidence to one invocation. */
    struct DecisionGraphInstance::StepScope final {
        DecisionGraphInstance &owner;
        StepObservation observation;

        StepScope(DecisionGraphInstance &instance, StepObservation input, DecisionStepTrace *evidence)
            : owner(instance), observation(input) {
            owner.evaluating_ = true;
            owner.input_ = &observation;
            owner.trace_ = evidence;
        }

        StepScope(const StepScope &) = delete;
        StepScope &operator=(const StepScope &) = delete;

        ~StepScope() {
            owner.input_ = nullptr;
            owner.trace_ = nullptr;
            owner.evaluating_ = false;
        }
    };

    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 std::shared_ptr<const DecisionNodeGeneration> providers,
                                                                                 DecisionGraphInstanceBinding binding) {
        return CreateBound(std::move(plan), std::move(providers), std::move(binding), {});
    }

    /** @copydoc DecisionGraphInstance::CreateBound */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::CreateBound(
        std::shared_ptr<const CookedDecisionPlan> plan, std::shared_ptr<const DecisionNodeGeneration> providers,
        DecisionGraphInstanceBinding binding, std::shared_ptr<IDecisionQueryTaskBinder> queryTasks) {
        if (auto admitted = Admit(plan, providers, binding); admitted.HasError())
            return Result<std::unique_ptr<DecisionGraphInstance>>::Failure(admitted.ErrorValue());
        try {
            auto result = std::make_unique<DecisionGraphInstance>();
            result->owner_ = std::this_thread::get_id();
            result->plan_ = std::move(plan);
            auto *binder = queryTasks.get();
            if (binder) {
                const auto *generation = providers.get();
                auto lease = std::make_shared<QueryProviderLease>(std::move(providers), std::move(queryTasks));
                result->providers_ = std::shared_ptr<const DecisionNodeGeneration>{std::move(lease), generation};
            } else
                result->providers_ = std::move(providers);
            result->binding_ = std::move(binding);
            result->usesUtility_ = std::ranges::any_of(result->plan_->Graphs(), [](const auto &graph) {
                return std::holds_alternative<std::shared_ptr<const CookedUtilityPlan>>(graph.execution);
            });
            result->occurrences_.reserve(result->plan_->Occurrences().size());
            for (std::size_t index = 0; index < result->plan_->Occurrences().size(); ++index) {
                result->occurrences_.push_back(std::make_unique<OccurrenceState>());
                if (auto built = result->BuildOccurrence(index, binder); built.HasError())
                    return Result<std::unique_ptr<DecisionGraphInstance>>::Failure(built.ErrorValue());
            }
            result->occurrences_[0]->active = true;
            result->occurrences_[0]->started = true;
            return Result<std::unique_ptr<DecisionGraphInstance>>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Invalid<std::unique_ptr<DecisionGraphInstance>>(AIErrors::DecisionAssetStorageUnavailable);
        }
    }

    Result<void> DecisionGraphInstance::Admit(const std::shared_ptr<const CookedDecisionPlan> &plan,
                                              const std::shared_ptr<const DecisionNodeGeneration> &providers,
                                              const DecisionGraphInstanceBinding &binding) {
        if (!plan || !providers || providers != plan->providers_ || !plan->IsCurrent() || !providers->CanAdmit())
            return Invalid<void>(AIErrors::RuntimeUnavailable);
        return AdmitBinding(binding, *plan);
    }

    AiTaskOperationContext DecisionGraphInstance::Operation(std::size_t index, TaskId task, std::uint32_t offset,
                                                            std::uint32_t generation) const {
        auto operation = binding_.operation;
        operation.taskDefinition = task;
        const auto slot = binding_.firstTaskSlot + static_cast<std::uint32_t>(plan_->Occurrences()[index].firstTaskSlot) + offset;
        operation.task = {operation.agent.incarnation, {slot, generation}};
        return operation;
    }

    Result<void> DecisionGraphInstance::BuildOccurrence(std::size_t index, IDecisionQueryTaskBinder *queryTasks) {
        auto &state = *occurrences_[index];
        const auto &graph = plan_->Graphs()[plan_->Occurrences()[index].plan];
        state.generation = binding_.firstTaskGeneration;
        if (const auto *tree = std::get_if<std::shared_ptr<const BehaviorTreeExecutionPlan>>(&graph.execution)) {
            auto callbacks = providers_->CreateExecutor(providers_, **tree, plan_->Capabilities(), graph.providerProperties, queryTasks,
                                                        &binding_.operation);
            if (callbacks.HasError())
                return Result<void>::Failure(callbacks.ErrorValue());
            BehaviorTreeInstanceBinding binding{binding_.operation.agent, binding_.operation.cancellation,
                                                binding_.firstTaskSlot +
                                                    static_cast<std::uint32_t>(plan_->Occurrences()[index].firstTaskSlot),
                                                binding_.firstTaskGeneration, binding_.blackboard};
            binding.capabilityCancellation = binding_.operation.capabilityCancellation;
            binding.diagnostics = binding_.operation.diagnostics;
            binding.ownerCancellation = binding_.operation.ownerCancellation;
            auto instance =
                BehaviorTreeInstance::Create(*tree, binding, std::make_unique<CallAdapter>(*this, index, std::move(callbacks).Value()));
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            state.tree = std::move(instance).Value();
        } else if (std::holds_alternative<std::shared_ptr<const CookedUtilityPlan>>(graph.execution)) {
            if (auto built = BuildUtility(index); built.HasError())
                return built;
        } else {
            auto callbacks = providers_->CreateExecutor(providers_, *graph.bindings, plan_->Capabilities(), graph.providerProperties,
                                                        &binding_.operation);
            if (callbacks.HasError())
                return Result<void>::Failure(callbacks.ErrorValue());
            state.actions = std::make_unique<ActionAdapter>(*this, index, std::move(callbacks).Value());
            if (auto prepared = state.actions->Prepare(); prepared.HasError())
                return prepared;
            auto instance = StateMachineInstance::Create(std::get<std::shared_ptr<const CookedStateMachinePlan>>(graph.execution),
                                                         binding_.blackboard, Operation(index, graph.executionTask, 0, state.generation));
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            state.machine = std::move(instance).Value();
        }
        return Available() ? Result<void>::Success() : Invalid<void>(AIErrors::RuntimeUnavailable);
    }

    bool DecisionGraphInstance::Available() const noexcept {
        return std::this_thread::get_id() == owner_ && !shutdown_ && providers_->CanAdmit() &&
               !binding_.operation.IsCancellationRequested();
    }

    /** @copydoc DecisionGraphInstance::HasPendingTaskWake */
    bool DecisionGraphInstance::HasPendingTaskWake() const noexcept {
        if (std::this_thread::get_id() != owner_ || shutdown_ || evaluating_ || binding_.operation.IsCancellationRequested())
            return false;
        if (!providers_->CanAdmit())
            return occurrences_[0]->outcome == AiTaskState::Running;
        return std::ranges::any_of(occurrences_, [](const auto &occurrence) {
            const auto *action = ActiveAction(*occurrence);
            return action && action->HasPendingTaskWake();
        });
    }

    /** @copydoc DecisionGraphInstance::CanDispatchIntents */
    bool DecisionGraphInstance::CanDispatchIntents() const noexcept {
        return Available() && !evaluating_;
    }

    /** @copydoc DecisionGraphInstance::NeedsUtilityPreference */
    bool DecisionGraphInstance::NeedsUtilityPreference() const noexcept {
        if (!CanDispatchIntents())
            return false;
        const auto &root = *occurrences_.front();
        return root.active && root.utility && !root.utility->selected && !Terminal(root.outcome);
    }

    /** @copydoc DecisionGraphInstance::NextFixedWakeTick */
    std::optional<std::uint64_t> DecisionGraphInstance::NextFixedWakeTick() const noexcept {
        if (!Available() || evaluating_)
            return std::nullopt;
        std::optional<std::uint64_t> earliest;
        for (const auto &occurrence : occurrences_) {
            const auto *action = ActiveAction(*occurrence);
            auto deadline = action ? action->NextFixedWakeTick() : std::nullopt;
            if (occurrence->active && occurrence->machine && occurrence->outcome == AiTaskState::Running &&
                occurrence->lastTick < std::numeric_limits<std::uint64_t>::max())
                deadline = occurrence->lastTick + 1;
            if (deadline.has_value() && (!earliest.has_value() || *deadline < *earliest))
                earliest = deadline;
        }
        return earliest;
    }

    /** @copydoc DecisionGraphInstance::ActiveAction */
    const BehaviorTreeInstance *DecisionGraphInstance::ActiveAction(const OccurrenceState &occurrence) noexcept {
        if (!occurrence.active)
            return nullptr;
        if (occurrence.utility && occurrence.utility->selected.has_value())
            return occurrence.utility->actions[*occurrence.utility->selected].get();
        return occurrence.tree.get();
    }

    namespace {
        /** @brief Compares exact Utility selection policy while retaining option-ID order. */
        bool SameUtilityOption(const UtilityOption &a, const UtilityOption &b) noexcept {
            return std::tie(a.id, a.action, a.priority, a.compensation) == std::tie(b.id, b.action, b.priority, b.compensation) &&
                   std::ranges::equal(a.considerations, b.considerations, [](const auto &left, const auto &right) {
                return std::tie(left.key, left.minimum, left.maximum, left.weight, left.curve, left.parameter, left.invert) ==
                       std::tie(right.key, right.minimum, right.maximum, right.weight, right.curve, right.parameter, right.invert);
            });
        }
    }  // namespace

    /** @copydoc DecisionGraphInstance::TryRetainCompatibleClosure */
    Result<bool> DecisionGraphInstance::TryRetainCompatibleClosure(const std::shared_ptr<const CookedDecisionPlan> &candidate) {
        using Retained = Result<bool>;
        if (!candidate || std::this_thread::get_id() != owner_ || evaluating_)
            return Invalid<bool>(AIErrors::DecisionAssetActivationInvalid);
        if (!Available() || candidate->Occurrences().size() != occurrences_.size() || candidate->Root() != plan_->Root() ||
            candidate->runtimeLayoutVersion_ != plan_->runtimeLayoutVersion_ || candidate->providers_ != plan_->providers_ ||
            candidate->providers_.owner_before(plan_->providers_) || plan_->providers_.owner_before(candidate->providers_) ||
            candidate->Capabilities() != plan_->Capabilities() || candidate->Storage().taskSlots != plan_->Storage().taskSlots)
            return Retained::Success(false);

        for (std::size_t index = 0; index < occurrences_.size(); ++index) {
            auto valid = ValidateRetainedOccurrence(index, *candidate);
            if (valid.HasError() || !valid.Value())
                return valid;
        }
        // Every occurrence is validated. Publishing the new plans needs no callbacks or storage.
        for (std::size_t index = 0; index < occurrences_.size(); ++index)
            PublishRetainedOccurrence(index, *candidate);
        plan_ = candidate;
        return Retained::Success(true);
    }

    /** @copydoc DecisionGraphInstance::ValidateRetainedOccurrence */
    Result<bool> DecisionGraphInstance::ValidateRetainedOccurrence(const std::size_t index, const CookedDecisionPlan &candidate) const {
        using Retained = Result<bool>;
        const auto &current = plan_->Occurrences()[index];
        const auto &replacement = candidate.Occurrences()[index];
        if (replacement.parent != current.parent || replacement.callNode != current.callNode ||
            replacement.subtreeEnd != current.subtreeEnd || replacement.firstTaskSlot != current.firstTaskSlot)
            return Retained::Success(false);

        const auto &existing = plan_->Graphs()[current.plan];
        const auto &next = candidate.Graphs()[replacement.plan];
        if (next.execution.index() != existing.execution.index() || next.asset != existing.asset ||
            next.schemaVersion != existing.schemaVersion || next.executionTask != existing.executionTask)
            return Retained::Success(false);

        if (!std::ranges::equal(existing.providerProperties, next.providerProperties, [](const auto &left, const auto &right) {
            return left.node == right.node && left.properties == right.properties;
        }))
            return Retained::Success(false);
        const auto admitTree = [](const BehaviorTreeInstance &live, const std::shared_ptr<const BehaviorTreeExecutionPlan> &tree) {
            if (!live.plan_->IsCompatible(*tree))
                return Retained::Success(false);
            if (auto admitted = live.AdmitReplacement(tree, nullptr); admitted.HasError())
                return Retained::Failure(admitted.ErrorValue());
            return Retained::Success(true);
        };
        const auto &state = *occurrences_[index];
        if (state.tree)
            return admitTree(*state.tree, std::get<std::shared_ptr<const BehaviorTreeExecutionPlan>>(next.execution));
        if (state.machine) {
            const auto &machine = std::get<std::shared_ptr<const CookedStateMachinePlan>>(next.execution);
            return Retained::Success(state.machine->plan_->IsCompatible(*machine));
        }

        using Utility = std::shared_ptr<const CookedUtilityPlan>;
        const auto &prior = *std::get<Utility>(existing.execution);
        const auto &utility = *std::get<Utility>(next.execution);
        const auto actions = utility.Actions();
        if (prior.Root() != utility.Root() || !existing.bindings->HasEquivalentBindings(*next.bindings) ||
            actions.size() != state.utility->actions.size() ||
            !std::ranges::equal(prior.Selector().Options(), utility.Selector().Options(), SameUtilityOption))
            return Retained::Success(false);
        for (std::size_t action = 0; action < actions.size(); ++action)
            if (auto admitted = admitTree(*state.utility->actions[action], actions[action]); admitted.HasError() || !admitted.Value())
                return admitted;
        return Retained::Success(true);
    }

    /** @copydoc DecisionGraphInstance::PublishRetainedOccurrence */
    void DecisionGraphInstance::PublishRetainedOccurrence(const std::size_t index, const CookedDecisionPlan &candidate) noexcept {
        const auto &execution = candidate.Graphs()[candidate.Occurrences()[index].plan].execution;
        auto &state = *occurrences_[index];
        if (state.tree) {
            state.tree->PublishRetainedPlan(std::get<std::shared_ptr<const BehaviorTreeExecutionPlan>>(execution));
        } else if (state.machine) {
            state.machine->plan_ = std::get<std::shared_ptr<const CookedStateMachinePlan>>(execution);
        } else {
            const auto actions = std::get<std::shared_ptr<const CookedUtilityPlan>>(execution)->Actions();
            for (std::size_t action = 0; action < actions.size(); ++action)
                state.utility->actions[action]->PublishRetainedPlan(actions[action]);
        }
    }

    /** @copydoc DecisionGraphInstance::TraceOccurrence */
    DecisionTraceOccurrence DecisionGraphInstance::TraceOccurrence(const std::size_t index) const noexcept {
        const auto &occurrence = plan_->Occurrences()[index];
        return {plan_->Graphs()[occurrence.plan].asset, occurrence.callNode, static_cast<std::uint32_t>(index),
                static_cast<std::uint32_t>(occurrence.parent)};
    }

    /** @copydoc DecisionGraphInstance::TraceMachine */
    void DecisionGraphInstance::TraceMachine(const std::size_t index, const StateMachineStepResult &step) const noexcept {
        const auto &state = *occurrences_[index];
        const auto &occurrence = plan_->Occurrences()[index];
        const auto task = plan_->Graphs()[occurrence.plan].executionTask;
        const auto slot = binding_.firstTaskSlot + static_cast<std::uint32_t>(occurrence.firstTaskSlot);
        trace_->Append({.kind = DecisionStepRecordKind::StateMachineStep,
                        .occurrence = TraceOccurrence(index),
                        .node = step.activeState,
                        .task = task,
                        .handle = {binding_.operation.agent.incarnation, {slot, state.generation}},
                        .state = state.outcome,
                        .lastTransition = step.lastTransition,
                        .transitionCount = static_cast<std::uint32_t>(step.transitions)});
    }

    /** @copydoc DecisionGraphInstance::TraceMachineTransitions */
    void DecisionGraphInstance::TraceMachineTransitions(const std::size_t index) const noexcept {
        const auto &state = *occurrences_[index];
        const auto &occurrence = plan_->Occurrences()[index];
        const auto slot = binding_.firstTaskSlot + static_cast<std::uint32_t>(occurrence.firstTaskSlot);
        std::uint32_t ordinal = 0;
        for (const auto transition : state.machine->CommittedTransitions()) {
            trace_->Append({.kind = DecisionStepRecordKind::StateMachineTransition,
                            .occurrence = TraceOccurrence(index),
                            .node = transition,
                            .task = plan_->Graphs()[occurrence.plan].executionTask,
                            .handle = {binding_.operation.agent.incarnation, {slot, state.generation}},
                            .transitionCount = ++ordinal});
        }
    }

    Result<AiTaskState> DecisionGraphInstance::Advance(std::size_t index) {
        auto &state = *occurrences_[index];
        if (!Available() || !state.active || !input_)
            return Invalid<AiTaskState>(AIErrors::RuntimeUnavailable);
        const auto &input = input_->values;
        if (Terminal(state.outcome) || state.lastTick == input.tick)
            return Result<AiTaskState>::Success(state.outcome);
        state.lastTick = input.tick;
        if (state.utility)
            return AdvanceUtility(index);
        if (!state.tree)
            return AdvanceMachine(index);
        auto stepped = state.tree->Evaluate(input.tick, input.blackboard, input.current.agent, input.treeResumeReason);
        if (trace_)
            state.tree->AppendTrace(TraceOccurrence(index), *trace_);
        if (stepped.HasError())
            return stepped;
        state.outcome = stepped.Value();
        return stepped;
    }

    /** @copydoc DecisionGraphInstance::AdvanceMachine */
    Result<AiTaskState> DecisionGraphInstance::AdvanceMachine(const std::size_t index) {
        auto &state = *occurrences_[index];
        const auto &input = input_->values;
        auto stepped = state.machine->Step(input.tick, input.current, input.blackboard, input.events, state.actions.get());
        if (trace_)
            TraceMachineTransitions(index);
        if (stepped.HasError())
            return Result<AiTaskState>::Failure(stepped.ErrorValue());
        const auto *terminal = state.machine->TerminalResult();
        state.outcome = terminal ? terminal->state : AiTaskState::Running;
        if (trace_)
            TraceMachine(index, stepped.Value());
        return Result<AiTaskState>::Success(state.outcome);
    }

    /** @copydoc DecisionGraphInstance::UtilityBudgetsAvailable */
    bool DecisionGraphInstance::UtilityBudgetsAvailable(const DecisionGraphStepInput &input) const noexcept {
        return !usesUtility_ || (input.utilityAgentBudget && input.utilityGlobalBudget);
    }

    Result<void> DecisionGraphInstance::ValidateStep(const DecisionGraphStepInput &input) const {
        if (std::this_thread::get_id() != owner_ || evaluating_ || shutdown_ || input.tick == 0 || input.tick <= lastTick_ ||
            input.treeResumeReason >= AiTaskResumeReason::Count)
            return Invalid<void>(AIErrors::TaskTransitionInvalid);
        const auto schema = plan_->Graphs()[plan_->Occurrences()[0].plan].bindings->BlackboardSchema();
        if (!MatchesBlackboard(input, binding_.blackboard, schema))
            return Invalid<void>(AIErrors::BlackboardInstanceStale);
        // Metadata still identifies this owner when revocation has expired the observation's read lease.
        if (binding_.operation.IsCancellationRequested())
            return Result<void>::Success();
        if (input.blackboard.Revision().HasError())
            return Invalid<void>(AIErrors::BlackboardInstanceStale);
        if (!UtilityBudgetsAvailable(input))
            return Invalid<void>(AIErrors::TaskContextInvalid);
        if (input.events.size() > StateMachineLimits::EventsPerStep)
            return Invalid<void>();
        if (!std::ranges::all_of(input.events, [](auto event) {
            return event.IsValid();
        }))
            return Invalid<void>();
        return Result<void>::Success();
    }

    Result<AiTaskState> DecisionGraphInstance::Step(const DecisionGraphStepInput &input) {
        return StepImpl(input, nullptr);
    }

    Result<AiTaskState> DecisionGraphInstance::StepWithLocalPolicy(const DecisionGraphStepInput &input,
                                                                   const LocalPolicyUtilityInput &policy) {
        if (auto valid = ValidateStep(input); valid.HasError())
            return Result<AiTaskState>::Failure(std::move(valid).ErrorValue());
        return StepRecorded(input, nullptr, nullptr, &policy);
    }

    /** @copydoc DecisionGraphInstance::StepBudgeted */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgeted(const DecisionGraphStepInput &input, AiWorkBudget &budget) {
        return StepBudgeted(input, budget, nullptr);
    }

    /** @copydoc DecisionGraphInstance::StepBudgetedWithLocalPolicy */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgetedWithLocalPolicy(const DecisionGraphStepInput &input,
                                                                                          AiWorkBudget &budget,
                                                                                          const LocalPolicyUtilityInput &policy,
                                                                                          DecisionStepTrace *trace) {
        return StepBudgetedWithLocalPolicy(input, budget, policy, trace, nullptr);
    }

    /** @copydoc DecisionGraphInstance::StepBudgetedWithLocalPolicy */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgetedWithLocalPolicy(
        const DecisionGraphStepInput &input, AiWorkBudget &budget, const LocalPolicyUtilityInput &policy,
        DecisionStepTrace *trace, std::optional<DecisionProviderFailure> *providerFailure) {
        const auto *admitted = &policy.work == &budget ? nullptr : &policy;
        return StepBudgetedImpl(input, budget, trace, providerFailure, admitted);
    }

    /** @copydoc DecisionGraphInstance::StepBudgeted */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgeted(const DecisionGraphStepInput &input, AiWorkBudget &budget,
                                                                           std::optional<DecisionProviderFailure> *providerFailure) {
        return StepBudgetedImpl(input, budget, nullptr, providerFailure);
    }

    /** @copydoc DecisionGraphInstance::StepBudgetedWithTrace */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgetedWithTrace(
        const DecisionGraphStepInput &input, AiWorkBudget &budget, DecisionStepTrace &trace,
        std::optional<DecisionProviderFailure> *providerFailure) {
        return StepBudgetedImpl(input, budget, &trace, providerFailure);
    }

    /** @copydoc DecisionGraphInstance::StepBudgetedImpl */
    Result<std::optional<AiTaskState>> DecisionGraphInstance::StepBudgetedImpl(const DecisionGraphStepInput &input, AiWorkBudget &budget,
                                                                               DecisionStepTrace *trace,
                                                                               std::optional<DecisionProviderFailure> *providerFailure,
                                                                               const LocalPolicyUtilityInput *policy) {
        using Outcome = Result<std::optional<AiTaskState>>;
        const auto work = plan_->MaximumDecisionWorkUnits();
        if (!work.has_value())
            return Outcome::Failure(MakeError(AIErrors::CapabilityUnavailable, "Cooked decision shape does not support scheduler slices."));
        auto observation = input;
        AiWorkBudget utilityWork{*work};
        if (usesUtility_) {
            observation.utilityAgentBudget = &utilityWork;
            observation.utilityGlobalBudget = &utilityWork;
        }
        if (auto valid = ValidateStep(observation); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (providerFailure)
            providerFailure->reset();
        if (Available() && !budget.Consume(*work))
            return Outcome::Success(std::nullopt);
        auto evaluated = StepRecorded(observation, trace, providerFailure, policy);
        if (providerFailure && (binding_.operation.IsCancellationRequested() || !providers_->CanAdmit()))
            providerFailure->reset();
        if (evaluated.HasError())
            return Outcome::Failure(evaluated.ErrorValue());
        return Outcome::Success(evaluated.Value());
    }

    /** @copydoc DecisionGraphInstance::StepWithTrace */
    Result<AiTaskState> DecisionGraphInstance::StepWithTrace(const DecisionGraphStepInput &input, DecisionStepTrace &trace) {
        return StepImpl(input, &trace);
    }

    /** @copydoc DecisionTraceHeader::MatchesObservation */
    bool DecisionTraceHeader::MatchesObservation(const Sha256Digest &digest, const std::uint64_t expectedTick,
                                                 const BlackboardInstanceBinding &binding, const std::uint64_t revision) const noexcept {
        return version == CurrentVersion && tick != 0 && tick == expectedTick && planDigest == digest && blackboard == binding &&
               blackboardRevision == revision;
    }

    /** @copydoc DecisionGraphInstance::StepImpl */
    Result<AiTaskState> DecisionGraphInstance::StepImpl(const DecisionGraphStepInput &input, DecisionStepTrace *trace) {
        if (auto valid = ValidateStep(input); valid.HasError())
            return Result<AiTaskState>::Failure(valid.ErrorValue());
        return StepRecorded(input, trace);
    }

    /** @copydoc DecisionGraphInstance::StepRecorded */
    Result<AiTaskState> DecisionGraphInstance::StepRecorded(const DecisionGraphStepInput &input, DecisionStepTrace *trace,
                                                            std::optional<DecisionProviderFailure> *providerFailure,
                                                            const LocalPolicyUtilityInput *policy) {
        if (trace) {
            trace->header_ = {.root = plan_->Root(),
                              .planDigest = plan_->Digest(),
                              .blackboard = input.blackboard.Binding(),
                              .tick = input.tick,
                              .blackboardRevision = input.blackboard.CapturedRevision()};
            trace->count_ = 0;
            trace->dropped_ = 0;
        }
        auto result = StepValidated(input, trace, providerFailure, policy);
        if (trace)
            trace->header_.outcome = result.HasValue() ? result.Value() : AiTaskState::Failed;
        return result;
    }

    /** @copydoc DecisionGraphInstance::StepValidated */
    Result<AiTaskState> DecisionGraphInstance::StepValidated(const DecisionGraphStepInput &input, DecisionStepTrace *trace,
                                                             std::optional<DecisionProviderFailure> *providerFailure,
                                                             const LocalPolicyUtilityInput *policy) {
        if (!Available()) {
            const auto previous = occurrences_[0]->outcome;
            static_cast<void>(Cancel(AiTaskCancellationReason::ContextCancelled));
            return Result<AiTaskState>::Success(Terminal(previous) ? previous : AiTaskState::Cancelled);
        }
        StepScope scope(*this, {input, providerFailure, policy}, trace);
        lastTick_ = input.tick;
        auto result = Advance(0);
        if (result.HasError() || !Available()) {
            StopOccurrences(AiTaskCancellationReason::Requested);
            shutdown_ = true;
        }
        if (result.HasValue() && !providers_->CanAdmit())
            return Invalid<AiTaskState>(AIErrors::RuntimeUnavailable);
        return result;
    }

    std::size_t DecisionGraphInstance::FindChild(std::size_t parent, DecisionNodeId node) const noexcept {
        for (std::size_t index = parent + 1; index < plan_->Occurrences()[parent].subtreeEnd; ++index) {
            const auto &candidate = plan_->Occurrences()[index];
            if (candidate.parent == parent && candidate.callNode == node)
                return index;
        }
        return occurrences_.size();
    }

    Result<void> DecisionGraphInstance::ResetOccurrence(std::size_t index) {
        const auto end = plan_->Occurrences()[index].subtreeEnd;
        for (std::size_t current = index; current < end; ++current) {
            auto &state = *occurrences_[current];
            if (state.tree) {
                if (auto restarted = state.tree->Restart(); restarted.HasError())
                    return restarted;
            } else if (state.utility) {
                for (auto &action : state.utility->actions) {
                    if (auto restarted = action->Restart(); restarted.HasError())
                        return restarted;
                }
                state.utility->selected.reset();
            } else {
                if (state.generation == std::numeric_limits<std::uint32_t>::max())
                    return Invalid<void>(AIErrors::GenerationExhausted);
                ++state.generation;
                const auto &graph = plan_->Graphs()[plan_->Occurrences()[current].plan];
                if (auto restarted = state.machine->Restart(Operation(current, graph.executionTask, 0, state.generation));
                    restarted.HasError())
                    return restarted;
            }
            state.lastTick = 0;
            state.outcome = AiTaskState::Idle;
            state.active = false;
        }
        return Result<void>::Success();
    }

    Result<void> DecisionGraphInstance::BeginCall(std::size_t parent, DecisionNodeId node) {
        const auto child = FindChild(parent, node);
        if (child == occurrences_.size())
            return Invalid<void>(AIErrors::DecisionAssetSubtreeMissing);
        auto &state = *occurrences_[child];
        if (state.started) {
            if (auto reset = ResetOccurrence(child); reset.HasError())
                return reset;
        }
        state.active = true;
        state.started = true;
        return Result<void>::Success();
    }

    Result<AiTaskState> DecisionGraphInstance::Call(std::size_t parent, DecisionNodeId node, bool entering) {
        if (entering) {
            if (auto begun = BeginCall(parent, node); begun.HasError())
                return Result<AiTaskState>::Failure(begun.ErrorValue());
        }
        const auto child = FindChild(parent, node);
        if (child == occurrences_.size())
            return Invalid<AiTaskState>(AIErrors::DecisionAssetSubtreeMissing);
        return Advance(child);
    }

    void DecisionGraphInstance::StopOccurrence(std::size_t index, AiTaskCancellationReason reason) noexcept {
        auto &state = *occurrences_[index];
        if (state.tree) {
            const auto &tree =
                *std::get<std::shared_ptr<const BehaviorTreeExecutionPlan>>(plan_->Graphs()[plan_->Occurrences()[index].plan].execution);
            static_cast<void>(state.tree->AbortSubtree(tree.Nodes()[0].execution.id, reason));
        }
        if (state.machine)
            static_cast<void>(state.machine->Cancel(reason, state.actions.get()));
        if (state.utility) {
            const auto &utility =
                *std::get<std::shared_ptr<const CookedUtilityPlan>>(plan_->Graphs()[plan_->Occurrences()[index].plan].execution);
            for (std::size_t action = 0; action < state.utility->actions.size(); ++action)
                static_cast<void>(state.utility->actions[action]->AbortSubtree(utility.Actions()[action]->Nodes()[0].execution.id, reason));
        }
        state.active = false;
        if (!Terminal(state.outcome))
            state.outcome = AiTaskState::Cancelled;
    }

    void DecisionGraphInstance::EndCall(std::size_t parent, DecisionNodeId node, AiTaskCancellationReason reason) noexcept {
        const auto child = FindChild(parent, node);
        if (child < occurrences_.size())
            StopOccurrence(child, reason);
    }

    /** @copydoc DecisionGraphInstance::StopOccurrences */
    void DecisionGraphInstance::StopOccurrences(AiTaskCancellationReason reason) noexcept {
        for (std::size_t index = occurrences_.size(); index > 0; --index)
            StopOccurrence(index - 1, reason);
    }

    /** @copydoc DecisionGraphInstance::SuspendForRelevance */
    Result<void> DecisionGraphInstance::SuspendForRelevance() {
        if (!Available() || evaluating_)
            return Invalid<void>(AIErrors::RuntimeUnavailable);
        auto &root = *occurrences_.front();
        if (root.outcome != AiTaskState::Running)
            return Result<void>::Success();

        StopOccurrences(AiTaskCancellationReason::Superseded);
        if (!Available()) {
            shutdown_ = true;
            return Invalid<void>(AIErrors::RuntimeUnavailable);
        }
        auto restarted = ResetOccurrence(0);
        if (restarted.HasError()) {
            shutdown_ = true;
            return restarted;
        }
        root.active = true;
        return Result<void>::Success();
    }

    Result<void> DecisionGraphInstance::Cancel(AiTaskCancellationReason reason) {
        if (std::this_thread::get_id() != owner_ || evaluating_ || reason >= AiTaskCancellationReason::Count)
            return Invalid<void>();
        if (shutdown_)
            return Result<void>::Success();
        StopOccurrences(reason);
        shutdown_ = true;
        return Result<void>::Success();
    }

    DecisionGraphInstance::~DecisionGraphInstance() {
        if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id())
            std::terminate();
        if (plan_)
            static_cast<void>(Cancel(AiTaskCancellationReason::OwnerShutdown));
    }

    std::uint64_t DecisionGraphInstance::CoreStorageBytes(const DecisionCompiledGraph &graph) noexcept {
        auto bytes = sizeof(OccurrenceState) + sizeof(std::unique_ptr<OccurrenceState>);
        if (std::holds_alternative<std::shared_ptr<const BehaviorTreeExecutionPlan>>(graph.execution))
            return bytes + sizeof(CallAdapter);
        if (const auto *utility = std::get_if<std::shared_ptr<const CookedUtilityPlan>>(&graph.execution))
            return bytes + sizeof(UtilityAdapter) + (*utility)->Actions().size() * sizeof(std::unique_ptr<BehaviorTreeInstance>);
        bytes += sizeof(ActionAdapter);
        for (const auto &node : graph.bindings->Nodes())
            if (node.intrinsic == DecisionControlKind::Unspecified)
                bytes += sizeof(ActionAdapter::Action) + sizeof(std::unique_ptr<ActionAdapter::Action>);
        return bytes;
    }
}  // namespace Horo::AI
