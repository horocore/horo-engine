#include "Horo/AI/StateMachine.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <type_traits>

namespace Horo::AI {
    namespace {
        /** @brief Compares validated scalar operands without conversion or floating-point epsilon ordering. */
        bool Compare(const BlackboardScalarValue &value, const StateMachineGuard &guard) {
            using enum StateMachineGuardOperator;
            if (value.index() != guard.operand.index())
                return false;
            switch (guard.operation) {
                case Equal:
                    return value == guard.operand;
                case NotEqual:
                    return value != guard.operand;
                case Less:
                case Greater:
                    return std::visit([&]<typename T>(const T &left) {
                        if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                            const T right = std::get<T>(guard.operand);
                            return guard.operation == Less ? left < right : left > right;
                        }
                        return false;
                    }, value);
                default:
                    return false;
            }
        }

        std::optional<TaskId> PhaseAction(const StateMachineState &state, StateMachineActionPhase phase) {
            switch (phase) {
                using enum Horo::AI::StateMachineActionPhase;
                case Entry:
                    return state.entry;
                case Update:
                    return state.update;
                case Exit:
                    return state.exit;
            }
            return {};
        }

        bool FreshRestart(const AiTaskOperationContext *previous, const AiTaskOperationContext &next) noexcept {
            return previous && next.taskDefinition == previous->taskDefinition && next.task.incarnation == previous->task.incarnation &&
                   next.task.slot.index == previous->task.slot.index && next.task.slot.generation > previous->task.slot.generation;
        }

        Result<bool> GuardHolds(const StateMachineGuard &guard, const BlackboardSnapshot &blackboard) {
            const auto read = blackboard.Read(guard.key);
            if (read.HasError())
                return Result<bool>::Failure(read.ErrorValue());
            if (guard.operation == StateMachineGuardOperator::Present)
                return Result<bool>::Success(read.Value().has_value());
            if (!read.Value())
                return Result<bool>::Success(false);
            const auto *scalar = std::get_if<BlackboardScalarValue>(&*read.Value());
            if (!scalar || scalar->index() != guard.operand.index())
                return Result<bool>::Failure(MakeError(AIErrors::BlackboardValueTypeMismatch));
            return Result<bool>::Success(Compare(*scalar, guard));
        }

        /** @brief Releases the reentrant evaluation fence even on typed failure. */
        struct EvaluationFence final {
            bool &active;
            StateMachineActions *&borrowed;

            EvaluationFence(bool &value, StateMachineActions *&slot, StateMachineActions *actions) : active(value), borrowed(slot) {
                active = true;
                borrowed = actions;
            }

            EvaluationFence(const EvaluationFence &) = delete;
            EvaluationFence &operator=(const EvaluationFence &) = delete;

            ~EvaluationFence() {
                borrowed = nullptr;
                active = false;
            }
        };
    }  // namespace

    /** @copydoc StateMachineInstance::Create */
    Result<std::unique_ptr<StateMachineInstance>> StateMachineInstance::Create(std::shared_ptr<const CookedStateMachinePlan> plan,
                                                                               const BlackboardInstanceBinding &binding,
                                                                               AiTaskOperationContext operation) {
        using Output = std::unique_ptr<StateMachineInstance>;
        if (plan == nullptr || !binding.IsValid() || binding.agent != operation.agent ||
            binding.schema != plan->Decision().BlackboardSchema()->Identity() ||
            binding.schemaVersion != plan->Decision().BlackboardSchema()->Version())
            return Result<Output>::Failure(MakeError(AIErrors::TaskContextInvalid));
        try {
            auto instance = std::make_unique<StateMachineInstance>(ConstructionToken{}, std::move(plan), binding);
            if (auto started = instance->lifecycle_.Start(std::move(operation)); started.HasError())
                return Result<Output>::Failure(std::move(started).ErrorValue());
            return Result<Output>::Success(std::move(instance));
        } catch (const std::bad_alloc &) {
            return Result<Output>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc StateMachineInstance::ActiveState */
    DecisionNodeId StateMachineInstance::ActiveState() const noexcept {
        return depth_ == 0 ? DecisionNodeId{} : plan_->source_.states[path_[depth_ - 1]].id;
    }

    /** @copydoc StateMachineInstance::Cancel */
    Result<AiTaskTransitionDisposition> StateMachineInstance::Cancel(const AiTaskCancellationReason reason, StateMachineActions *actions) {
        if (evaluating_)
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(StateMachineErrors::StepInvalid));
        if (depth_ != 0 && !HasRequiredActions(actions))
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto result = lifecycle_.RequestCancellation(reason);
        if (result.HasValue()) {
            StopCalls(path_, 0, depth_, actions, reason);
            depth_ = 0;
        }
        return result;
    }

    Result<void> StateMachineInstance::Restart(AiTaskOperationContext operation) {
        if (evaluating_ || depth_ != 0 || operation.agent != binding_.agent)
            return Result<void>::Failure(MakeError(StateMachineErrors::StepInvalid));
        const auto *previous = lifecycle_.Context();
        if (!FreshRestart(previous, operation))
            return Result<void>::Failure(MakeError(AIErrors::GenerationExhausted));
        auto cancelled = lifecycle_.RequestCancellation(AiTaskCancellationReason::Superseded);
        if (cancelled.HasError())
            return Result<void>::Failure(cancelled.ErrorValue());
        std::destroy_at(&lifecycle_);
        std::construct_at(&lifecycle_);
        auto started = lifecycle_.Start(std::move(operation));
        if (started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        childOutcomes_.fill(AiTaskState::Idle);
        lastTick_.reset();
        committedTransitionCount_ = 0;
        return Result<void>::Success();
    }

    DecisionKernelStorage CookedStateMachinePlan::Storage() const noexcept {
        DecisionKernelStorage storage;
        storage.compiledBytes = sizeof(CookedStateMachinePlan) + source_.states.capacity() * sizeof(StateMachineState) +
                                source_.transitions.capacity() * sizeof(StateMachineTransition);
        storage.instanceBytes = sizeof(StateMachineInstance);
        storage.scratchBytes = 3 * sizeof(StateMachineInstance::Path) + sizeof(std::array<bool, StateMachineLimits::States>);
        storage.taskSlots = 1;
        for (const auto &state : source_.states) {
            std::size_t depth = 1;
            auto parent = state.parent;
            while (parent.IsValid()) {
                ++depth;
                parent = source_.states[Index(parent)].parent;
            }
            storage.maximumDepth = std::max(storage.maximumDepth, depth);
        }
        return storage;
    }

    /** @brief Builds an ancestor-first path and follows every declared initial child. */
    std::size_t StateMachineInstance::MakePath(DecisionNodeId target, Path &path) const noexcept {
        auto index = plan_->Index(target);
        while (plan_->source_.states[index].initialChild.IsValid())
            index = plan_->Index(plan_->source_.states[index].initialChild);
        std::size_t depth{};
        while (true) {
            path[depth++] = index;
            const auto parent = plan_->source_.states[index].parent;
            if (!parent.IsValid())
                break;
            index = plan_->Index(parent);
        }
        std::reverse(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(depth));
        return depth;
    }

    /** @brief Revalidates cancellation and snapshot lifetime before actions and logical publication. */
    Result<void> StateMachineInstance::CheckBoundary(const BlackboardSnapshot &blackboard) {
        const auto resumed = lifecycle_.PrepareResume(AiTaskResumeReason::FixedTick, binding_.agent);
        if (resumed.HasError())
            return Result<void>::Failure(resumed.ErrorValue());
        if (resumed.Value() != AiTaskResumeDisposition::Ready)
            return Result<void>::Failure(MakeError(StateMachineErrors::StepInvalid));
        if (const auto revision = blackboard.Revision(); revision.HasError())
            return Result<void>::Failure(revision.ErrorValue());
        return Result<void>::Success();
    }

    void StateMachineInstance::StopCalls(const Path &path, std::size_t begin, std::size_t end, StateMachineActions *actions,
                                         AiTaskCancellationReason reason) noexcept {
        if (!actions)
            return;
        for (std::size_t offset = end; offset > begin; --offset) {
            const auto index = path[offset - 1];
            if (plan_->source_.states[index].call)
                actions->CancelState(plan_->source_.states[index].id, reason);
            childOutcomes_[index] = AiTaskState::Idle;
        }
    }

    Result<void> StateMachineInstance::RunCall(const StateMachineState &state, std::size_t index, StateMachineActionPhase phase,
                                               const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions) {
        using enum Horo::AI::AiTaskState;
        if (!state.call)
            return Result<void>::Success();
        if (!actions || !actions->SupportsSubplans())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSubtreeIncompatible));
        const StateMachineStateInvocation invocation{state.id, *lifecycle_.Context(), blackboard, tick};
        if (phase == StateMachineActionPhase::Entry) {
            childOutcomes_[index] = Idle;
            return actions->EnterState(invocation);
        }
        if (phase == StateMachineActionPhase::Exit) {
            childOutcomes_[index] = Idle;
            return actions->ExitState(invocation);
        }
        auto result = actions->UpdateState(invocation);
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (result.Value() < Running || result.Value() > Cancelled)
            return Result<void>::Failure(MakeError(AIErrors::TaskFailureInvalid));
        childOutcomes_[index] = result.Value();
        return Result<void>::Success();
    }

    Result<void> StateMachineInstance::InvokePhase(const StateMachineState &state, StateMachineActionPhase phase,
                                                   const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions) {
        if (auto ready = CheckBoundary(blackboard); ready.HasError())
            return ready;
        const auto action = PhaseAction(state, phase);
        if (action) {
            if (!actions)
                return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
            auto invoked = actions->Invoke({state.id, *action, phase, *lifecycle_.Context(), blackboard, tick});
            if (invoked.HasError())
                return invoked;
        }
        return CheckBoundary(blackboard);
    }

    Result<void> StateMachineInstance::RunState(const StateMachineState &state, std::size_t index, StateMachineActionPhase phase,
                                                const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions) {
        if (auto ready = CheckBoundary(blackboard); ready.HasError())
            return ready;
        if (phase == StateMachineActionPhase::Exit) {
            if (auto exited = RunCall(state, index, phase, blackboard, tick, actions); exited.HasError())
                return exited;
        }
        if (auto invoked = InvokePhase(state, phase, blackboard, tick, actions); invoked.HasError())
            return invoked;
        if (phase != StateMachineActionPhase::Exit)
            return RunCall(state, index, phase, blackboard, tick, actions);
        return Result<void>::Success();
    }

    Result<void> StateMachineInstance::RunActions(const Path &path, std::size_t begin, std::size_t end, StateMachineActionPhase phase,
                                                  const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions) {
        for (std::size_t offset = begin; offset < end; ++offset) {
            const auto index = phase == StateMachineActionPhase::Exit ? path[end - 1 - (offset - begin)] : path[offset];
            auto result = RunState(plan_->source_.states[index], index, phase, blackboard, tick, actions);
            if (result.HasError()) {
                StopCalls(path, begin, end, actions, AiTaskCancellationReason::Requested);
                return result;
            }
        }
        return Result<void>::Success();
    }

    /** @brief Reads only shared compiler-admitted keys; absent optional values never satisfy ordinary comparisons. */
    Result<bool> StateMachineInstance::GuardsHold(const StateMachineTransition &transition, const BlackboardSnapshot &blackboard) const {
        if (transition.childOutcome && childOutcomes_[plan_->Index(transition.source)] != *transition.childOutcome)
            return Result<bool>::Success(false);
        for (const auto &guard : transition.guards) {
            auto accepted = GuardHolds(guard, blackboard);
            if (accepted.HasError())
                return accepted;
            if (!accepted.Value())
                return Result<bool>::Success(false);
        }
        return Result<bool>::Success(true);
    }

    /** @brief Selects one leaf-most source, then highest priority and stable-ID transition. */
    Result<const StateMachineTransition *> StateMachineInstance::Select(const BlackboardSnapshot &blackboard,
                                                                        const std::span<const StateMachineEventId> events) const {
        for (std::size_t depth = depth_; depth > 0; --depth) {
            const auto source = plan_->source_.states[path_[depth - 1]].id;
            for (const auto &transition : plan_->source_.transitions) {
                if (transition.source != source || (transition.event && std::ranges::find(events, *transition.event) == events.end()))
                    continue;
                const auto accepted = GuardsHold(transition, blackboard);
                if (accepted.HasError())
                    return Result<const StateMachineTransition *>::Failure(accepted.ErrorValue());
                if (accepted.Value())
                    return Result<const StateMachineTransition *>::Success(&transition);
            }
        }
        return Result<const StateMachineTransition *>::Success(nullptr);
    }

    /** @brief Applies LCA exits and candidate entries; failure clears the path through the shared terminal boundary. */
    Result<void> StateMachineInstance::Enter(const DecisionNodeId target, const BlackboardSnapshot &blackboard, const std::uint64_t tick,
                                             StateMachineActions *const actions, const std::optional<std::size_t> source) {
        Path candidate{};
        const auto candidateDepth = MakePath(target, candidate);
        std::size_t common{};
        while (common < depth_ && common < candidateDepth && path_[common] == candidate[common])
            ++common;
        // External transitions from an ancestor re-enter it even if its selected leaf is unchanged.
        if (source.has_value()) {
            const auto position = std::find(path_.begin(), path_.begin() + static_cast<std::ptrdiff_t>(depth_), *source);
            common = std::min(common, static_cast<std::size_t>(position - path_.begin()));
        }
        if (auto exited = RunActions(path_, common, depth_, StateMachineActionPhase::Exit, blackboard, tick, actions); exited.HasError())
            return exited;
        if (auto entered = RunActions(candidate, common, candidateDepth, StateMachineActionPhase::Entry, blackboard, tick, actions);
            entered.HasError())
            return entered;
        if (auto ready = CheckBoundary(blackboard); ready.HasError())
            return ready;
        path_ = candidate;
        depth_ = candidateDepth;
        return Result<void>::Success();
    }

    /** @brief Retains the original typed cause and makes a partial entry/exit unambiguously terminal. */
    Result<StateMachineStepResult> StateMachineInstance::Fail(Error error) {
        StopCalls(path_, 0, depth_, actions_, AiTaskCancellationReason::Requested);
        const auto completed = lifecycle_.CompleteFailure(binding_.agent, {AiTaskFailureKind::Execution, error});
        depth_ = 0;
        if (completed.HasError())
            return Result<StateMachineStepResult>::Failure(completed.ErrorValue());
        return Result<StateMachineStepResult>::Failure(std::move(error));
    }

    /** @brief Validates the bounded once-per-tick admission contract without consuming a step. */
    bool StateMachineInstance::ValidStep(const std::uint64_t tick, const std::span<const StateMachineEventId> events) const noexcept {
        return !evaluating_ && (!lastTick_.has_value() || tick > *lastTick_) && events.size() <= StateMachineLimits::EventsPerStep &&
               std::ranges::all_of(events, [](const auto event) {
            return event.IsValid();
        });
    }

    /** @brief Matches every blackboard publication fence and verifies its retained generation lease. */
    bool StateMachineInstance::MatchesInput(const BlackboardInstanceBinding &current, const BlackboardSnapshot &blackboard) const {
        return current == binding_ && blackboard.Binding() == current && blackboard.Revision().HasValue();
    }

    /** @brief Requires a synchronous adapter before an action-bearing plan can start. */
    bool StateMachineInstance::HasRequiredActions(const StateMachineActions *actions) const noexcept {
        const bool calls = std::ranges::any_of(plan_->source_.states, [](const auto &state) {
            return state.call.has_value();
        });
        if (calls && (!actions || !actions->SupportsSubplans()))
            return false;
        return actions || std::ranges::none_of(plan_->source_.states, [](const auto &state) {
            return state.entry || state.update || state.exit;
        });
    }

    Result<bool> StateMachineInstance::FinishTerminal(StateMachineActions *actions) {
        const auto terminal = plan_->source_.states[path_[depth_ - 1]].terminal;
        if (terminal == DecisionStateTerminal::None)
            return Result<bool>::Success(false);
        StopCalls(path_, 0, depth_, actions, AiTaskCancellationReason::Requested);
        auto completed = terminal == DecisionStateTerminal::Succeeded
                             ? lifecycle_.CompleteSuccess(binding_.agent)
                             : lifecycle_.CompleteFailure(binding_.agent,
                                                          {AiTaskFailureKind::Execution, MakeError(StateMachineErrors::AuthoredFailure)});
        if (completed.HasError())
            return Result<bool>::Failure(completed.ErrorValue());
        depth_ = 0;
        return Result<bool>::Success(true);
    }

    Result<bool> StateMachineInstance::PreparePath(const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions) {
        if (depth_ == 0) {
            if (auto entered = Enter(plan_->source_.initial, blackboard, tick, actions); entered.HasError())
                return Result<bool>::Failure(entered.ErrorValue());
        }
        return FinishTerminal(actions);
    }

    Result<bool> StateMachineInstance::CommitTransition(const StateMachineTransition &transition, const BlackboardSnapshot &blackboard,
                                                        std::uint64_t tick, StateMachineActions *actions) {
        if (auto entered = Enter(transition.target, blackboard, tick, actions, plan_->Index(transition.source)); entered.HasError())
            return Result<bool>::Failure(entered.ErrorValue());
        return FinishTerminal(actions);
    }

    /** @copydoc StateMachineInstance::Step */
    Result<StateMachineStepResult> StateMachineInstance::Step(const std::uint64_t tick, const BlackboardInstanceBinding &current,
                                                              const BlackboardSnapshot &blackboard,
                                                              const std::span<const StateMachineEventId> events,
                                                              StateMachineActions *const actions) {
        if (!ValidStep(tick, events))
            return Result<StateMachineStepResult>::Failure(MakeError(StateMachineErrors::StepInvalid));
        committedTransitionCount_ = 0;
        EvaluationFence fence(evaluating_, actions_, actions);
        const auto resumed = lifecycle_.PrepareResume(AiTaskResumeReason::FixedTick, current.agent);
        if (resumed.HasError())
            return Result<StateMachineStepResult>::Failure(resumed.ErrorValue());
        if (resumed.Value() != AiTaskResumeDisposition::Ready) {
            StopCalls(path_, 0, depth_, actions, AiTaskCancellationReason::ContextCancelled);
            depth_ = 0;
            return Result<StateMachineStepResult>::Success({StateMachineStepDisposition::Terminal});
        }
        if (!MatchesInput(current, blackboard))
            return Fail(MakeError(AIErrors::BlackboardInstanceStale));
        if (!HasRequiredActions(actions))
            return Result<StateMachineStepResult>::Failure(MakeError(AIErrors::TaskContextInvalid));
        lastTick_ = tick;
        auto terminal = PreparePath(blackboard, tick, actions);
        if (terminal.HasError())
            return Fail(terminal.ErrorValue());
        if (terminal.Value())
            return Result<StateMachineStepResult>::Success({StateMachineStepDisposition::Terminal});
        if (auto updated = RunActions(path_, 0, depth_, StateMachineActionPhase::Update, blackboard, tick, actions); updated.HasError())
            return Fail(std::move(updated).ErrorValue());
        return AdvanceTransitions(tick, blackboard, events, actions);
    }

    /** @brief Executes the explicit transition budget and detects same-step cycles before repeated entry. */
    Result<StateMachineStepResult> StateMachineInstance::AdvanceTransitions(const std::uint64_t tick, const BlackboardSnapshot &blackboard,
                                                                            const std::span<const StateMachineEventId> events,
                                                                            StateMachineActions *const actions) {
        std::array<bool, StateMachineLimits::States> visited{};
        visited[path_[depth_ - 1]] = true;
        StateMachineStepResult result{.activeState = ActiveState()};
        for (std::size_t count = 0; count < plan_->source_.maximumTransitionsPerStep; ++count) {
            const auto selected = Select(blackboard, count == 0 ? events : std::span<const StateMachineEventId>{});
            if (selected.HasError())
                return Fail(selected.ErrorValue());
            const auto *transition = selected.Value();
            if (transition == nullptr)
                return Result<StateMachineStepResult>::Success(result);
            Path candidate{};
            const auto candidateDepth = MakePath(transition->target, candidate);
            const auto nextLeaf = candidate[candidateDepth - 1];
            if (count != 0 && visited[nextLeaf]) {
                result.disposition = StateMachineStepDisposition::CycleBounded;
                result.lastTransition = transition->id;
                return Result<StateMachineStepResult>::Success(result);
            }
            auto terminal = CommitTransition(*transition, blackboard, tick, actions);
            if (terminal.HasError())
                return Fail(terminal.ErrorValue());
            committedTransitions_[committedTransitionCount_++] = transition->id;
            if (terminal.Value())
                return Result<StateMachineStepResult>::Success({StateMachineStepDisposition::Terminal, {}, transition->id, count + 1});
            visited[nextLeaf] = true;
            result = {StateMachineStepDisposition::Transitioned, ActiveState(), transition->id, count + 1};
        }
        if (plan_->source_.maximumTransitionsPerStep > 1)
            result.disposition = StateMachineStepDisposition::BudgetBounded;
        return Result<StateMachineStepResult>::Success(result);
    }
}  // namespace Horo::AI
