#include "DecisionInstanceState.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::TaskContextInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        bool Terminal(AiTaskState state) noexcept {
            using enum Horo::AI::AiTaskState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

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

        struct StepScope final {
            bool &evaluating;
            const DecisionGraphStepInput *&current;
            DecisionStepTrace *&trace;

            StepScope(bool &flag, const DecisionGraphStepInput *&slot, DecisionStepTrace *&traceSlot, const DecisionGraphStepInput &input,
                      DecisionStepTrace *evidence)
                : evaluating(flag), current(slot), trace(traceSlot) {
                evaluating = true;
                current = &input;
                trace = evidence;
            }

            StepScope(const StepScope &) = delete;
            StepScope &operator=(const StepScope &) = delete;

            ~StepScope() {
                current = nullptr;
                trace = nullptr;
                evaluating = false;
            }
        };
    }  // namespace

    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 std::shared_ptr<const DecisionNodeGeneration> providers,
                                                                                 DecisionGraphInstanceBinding binding) {
        if (auto admitted = Admit(plan, providers, binding); admitted.HasError())
            return Result<std::unique_ptr<DecisionGraphInstance>>::Failure(admitted.ErrorValue());
        try {
            auto result = std::make_unique<DecisionGraphInstance>();
            result->owner_ = std::this_thread::get_id();
            result->plan_ = std::move(plan);
            result->providers_ = std::move(providers);
            result->binding_ = std::move(binding);
            result->usesUtility_ = std::ranges::any_of(result->plan_->Graphs(), [](const auto &graph) {
                return std::holds_alternative<std::shared_ptr<const CookedUtilityPlan>>(graph.execution);
            });
            result->occurrences_.reserve(result->plan_->Occurrences().size());
            for (std::size_t index = 0; index < result->plan_->Occurrences().size(); ++index) {
                result->occurrences_.push_back(std::make_unique<OccurrenceState>());
                if (auto built = result->BuildOccurrence(index); built.HasError())
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

    Result<void> DecisionGraphInstance::BuildOccurrence(std::size_t index) {
        auto &state = *occurrences_[index];
        const auto &graph = plan_->Graphs()[plan_->Occurrences()[index].plan];
        state.generation = binding_.firstTaskGeneration;
        if (const auto *tree = std::get_if<std::shared_ptr<const BehaviorTreeExecutionPlan>>(&graph.execution)) {
            auto callbacks = providers_->CreateExecutor(providers_, **tree, plan_->Capabilities(), graph.providerProperties);
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
            auto callbacks = providers_->CreateExecutor(providers_, *graph.bindings, plan_->Capabilities(), graph.providerProperties);
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
        return !shutdown_ && std::this_thread::get_id() == owner_ && providers_->CanAdmit() &&
               !binding_.operation.IsCancellationRequested();
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
        if (Terminal(state.outcome) || state.lastTick == input_->tick)
            return Result<AiTaskState>::Success(state.outcome);
        state.lastTick = input_->tick;
        if (state.utility)
            return AdvanceUtility(index);
        if (!state.tree)
            return AdvanceMachine(index);
        auto stepped = state.tree->Evaluate(input_->tick, input_->blackboard, input_->current.agent);
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
        auto stepped = state.machine->Step(input_->tick, input_->current, input_->blackboard, input_->events, state.actions.get());
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
        if (std::this_thread::get_id() != owner_ || evaluating_ || shutdown_ || input.tick == 0 || input.tick <= lastTick_)
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

    /** @copydoc DecisionGraphInstance::StepWithTrace */
    Result<AiTaskState> DecisionGraphInstance::StepWithTrace(const DecisionGraphStepInput &input, DecisionStepTrace &trace) {
        return StepImpl(input, &trace);
    }

    /** @copydoc DecisionGraphInstance::StepImpl */
    Result<AiTaskState> DecisionGraphInstance::StepImpl(const DecisionGraphStepInput &input, DecisionStepTrace *trace) {
        if (auto valid = ValidateStep(input); valid.HasError())
            return Result<AiTaskState>::Failure(valid.ErrorValue());
        if (trace) {
            trace->header_ = {.root = plan_->Root(),
                              .planDigest = plan_->Digest(),
                              .agent = input.current.agent,
                              .tick = input.tick,
                              .blackboardRevision = input.blackboard.CapturedRevision()};
            trace->count_ = 0;
            trace->dropped_ = 0;
        }
        auto result = StepValidated(input, trace);
        if (trace)
            trace->header_.outcome = result.HasValue() ? result.Value() : AiTaskState::Failed;
        return result;
    }

    /** @copydoc DecisionGraphInstance::StepValidated */
    Result<AiTaskState> DecisionGraphInstance::StepValidated(const DecisionGraphStepInput &input, DecisionStepTrace *trace) {
        if (!Available()) {
            const auto previous = occurrences_[0]->outcome;
            static_cast<void>(Cancel(AiTaskCancellationReason::ContextCancelled));
            return Result<AiTaskState>::Success(Terminal(previous) ? previous : AiTaskState::Cancelled);
        }
        StepScope scope(evaluating_, input_, trace_, input, trace);
        lastTick_ = input.tick;
        auto result = Advance(0);
        if (result.HasError() || !Available()) {
            for (std::size_t index = occurrences_.size(); index > 0; --index)
                StopOccurrence(index - 1, AiTaskCancellationReason::Requested);
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

    Result<void> DecisionGraphInstance::Cancel(AiTaskCancellationReason reason) {
        if (std::this_thread::get_id() != owner_ || evaluating_ || reason >= AiTaskCancellationReason::Count)
            return Invalid<void>();
        if (shutdown_)
            return Result<void>::Success();
        for (std::size_t index = occurrences_.size(); index > 0; --index)
            StopOccurrence(index - 1, reason);
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
