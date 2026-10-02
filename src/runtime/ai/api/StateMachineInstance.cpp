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

        /** @brief Releases the reentrant evaluation fence even on typed failure. */
        struct EvaluationFence final {
            bool &active;

            explicit EvaluationFence(bool &value) : active(value) {
                value = true;
            }

            EvaluationFence(const EvaluationFence &) = delete;
            EvaluationFence &operator=(const EvaluationFence &) = delete;

            ~EvaluationFence() {
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
    Result<AiTaskTransitionDisposition> StateMachineInstance::Cancel(const AiTaskCancellationReason reason) {
        if (evaluating_)
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(StateMachineErrors::StepInvalid));
        auto result = lifecycle_.RequestCancellation(reason);
        if (result.HasValue())
            depth_ = 0;
        return result;
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

    /** @brief Executes bounded one-shot actions in the phase's declared hierarchy order. */
    Result<void> StateMachineInstance::RunActions(const Path &path, const std::size_t begin, const std::size_t end,
                                                  const StateMachineActionPhase phase, const BlackboardSnapshot &blackboard,
                                                  const std::uint64_t tick, StateMachineActions *const actions) {
        using enum StateMachineActionPhase;
        for (std::size_t offset = begin; offset < end; ++offset) {
            const auto index = phase == Exit ? end - 1 - (offset - begin) : offset;
            const auto &state = plan_->source_.states[path[index]];
            std::optional<TaskId> action = state.update;
            if (phase == Entry)
                action = state.entry;
            else if (phase == Exit)
                action = state.exit;
            if (!action)
                continue;
            auto ready = CheckBoundary(blackboard);
            if (ready.HasError())
                return ready;
            if (auto invoked = actions->Invoke({state.id, *action, phase, *lifecycle_.Context(), blackboard, tick}); invoked.HasError())
                return invoked;
            ready = CheckBoundary(blackboard);
            if (ready.HasError())
                return ready;
        }
        return Result<void>::Success();
    }

    /** @brief Reads only shared compiler-admitted keys; absent optional values never satisfy ordinary comparisons. */
    Result<bool> StateMachineInstance::GuardsHold(const StateMachineTransition &transition, const BlackboardSnapshot &blackboard) const {
        for (const auto &guard : transition.guards) {
            const auto read = blackboard.Read(guard.key);
            if (read.HasError())
                return Result<bool>::Failure(read.ErrorValue());
            if (guard.operation == StateMachineGuardOperator::Present) {
                if (!read.Value())
                    return Result<bool>::Success(false);
                continue;
            }
            if (!read.Value())
                return Result<bool>::Success(false);
            const auto *scalar = std::get_if<BlackboardScalarValue>(&*read.Value());
            if (scalar == nullptr || scalar->index() != guard.operand.index())
                return Result<bool>::Failure(MakeError(AIErrors::BlackboardValueTypeMismatch));
            if (!Compare(*scalar, guard))
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
    bool StateMachineInstance::HasRequiredActions(const StateMachineActions *const actions) const noexcept {
        return actions != nullptr || std::ranges::none_of(plan_->source_.states, [](const auto &state) {
            return state.entry || state.update || state.exit;
        });
    }

    /** @copydoc StateMachineInstance::Step */
    Result<StateMachineStepResult> StateMachineInstance::Step(const std::uint64_t tick, const BlackboardInstanceBinding &current,
                                                              const BlackboardSnapshot &blackboard,
                                                              const std::span<const StateMachineEventId> events,
                                                              StateMachineActions *const actions) {
        if (!ValidStep(tick, events))
            return Result<StateMachineStepResult>::Failure(MakeError(StateMachineErrors::StepInvalid));
        EvaluationFence fence(evaluating_);
        const auto resumed = lifecycle_.PrepareResume(AiTaskResumeReason::FixedTick, current.agent);
        if (resumed.HasError())
            return Result<StateMachineStepResult>::Failure(resumed.ErrorValue());
        if (resumed.Value() != AiTaskResumeDisposition::Ready) {
            depth_ = 0;
            return Result<StateMachineStepResult>::Success({StateMachineStepDisposition::Terminal});
        }
        if (!MatchesInput(current, blackboard))
            return Fail(MakeError(AIErrors::BlackboardInstanceStale));
        if (!HasRequiredActions(actions))
            return Result<StateMachineStepResult>::Failure(MakeError(AIErrors::TaskContextInvalid));
        lastTick_ = tick;
        if (depth_ == 0) {
            if (auto entered = Enter(plan_->source_.initial, blackboard, tick, actions); entered.HasError())
                return Fail(std::move(entered).ErrorValue());
        }
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
            if (auto entered = Enter(transition->target, blackboard, tick, actions, plan_->Index(transition->source)); entered.HasError())
                return Fail(std::move(entered).ErrorValue());
            visited[nextLeaf] = true;
            result = {StateMachineStepDisposition::Transitioned, ActiveState(), transition->id, count + 1};
        }
        if (plan_->source_.maximumTransitionsPerStep > 1)
            result.disposition = StateMachineStepDisposition::BudgetBounded;
        return Result<StateMachineStepResult>::Success(result);
    }
}  // namespace Horo::AI
