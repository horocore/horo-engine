#include "AITaskMailbox.h"
#include "BehaviorTreeInstanceState.h"
#include "Horo/AI/BehaviorTreeRuntime.h"

#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Recognizes the shared task/subtree terminal vocabulary. */
        bool Terminal(const AiTaskState state) noexcept {
            using enum AiTaskState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        /** @brief Produces a stable input/lifecycle rejection. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Releases the reentrancy guard on every return path. */
        struct EvaluationGuard final {
            bool &active;

            explicit EvaluationGuard(bool &flag) : active(flag) {
                flag = true;
            }

            ~EvaluationGuard() {
                active = false;
            }

            EvaluationGuard(const EvaluationGuard &) = delete;
            EvaluationGuard &operator=(const EvaluationGuard &) = delete;
            EvaluationGuard(EvaluationGuard &&) = delete;
            EvaluationGuard &operator=(EvaluationGuard &&) = delete;
        };

        static_assert(!std::is_copy_constructible_v<EvaluationGuard>);
        static_assert(!std::is_copy_assignable_v<EvaluationGuard>);
        static_assert(!std::is_move_constructible_v<EvaluationGuard>);
        static_assert(!std::is_move_assignable_v<EvaluationGuard>);
    }  // namespace

    /** @copydoc BehaviorTreeInstance::BehaviorTreeInstance */
    BehaviorTreeInstance::BehaviorTreeInstance(ConstructionKey) {}

    /** @copydoc BehaviorTreeInstance::Create */
    Result<std::unique_ptr<BehaviorTreeInstance>> BehaviorTreeInstance::Create(std::shared_ptr<const BehaviorTreeExecutionPlan> plan,
                                                                               BehaviorTreeInstanceBinding binding,
                                                                               std::unique_ptr<IBehaviorTreeExecutor> executor) {
        if (!plan || !executor || !binding.agent.IsValid() || !binding.blackboard.IsValid() || binding.blackboard.agent != binding.agent ||
            binding.blackboard.schema != plan->Bindings().BlackboardSchema()->Identity() ||
            binding.blackboard.schemaVersion != plan->Bindings().BlackboardSchema()->Version() || binding.firstTaskGeneration == 0 ||
            binding.firstTaskSlot > std::numeric_limits<std::uint32_t>::max() - BehaviorTreeExecutionLimits::HardNodes)
            return Failure<std::unique_ptr<BehaviorTreeInstance>>(AIErrors::TaskContextInvalid);
        try {
            auto instance = std::make_unique<BehaviorTreeInstance>(ConstructionKey{});
            instance->states_ = std::make_unique<NodeState[]>(plan->Nodes().size());
            instance->services_ = std::make_unique<ServiceState[]>(plan->Services().size());
            instance->frames_ = std::make_unique<Frame[]>(plan->Depth());
            instance->mailbox_ = std::make_shared<Detail::AiTaskMailbox>(BehaviorTreeExecutionLimits::HardNodes);
            instance->binding_ = std::move(binding);
            instance->nextGeneration_ = instance->binding_.firstTaskGeneration;
            instance->executor_ = std::move(executor);
            instance->plan_ = std::move(plan);
            return Result<std::unique_ptr<BehaviorTreeInstance>>::Success(std::move(instance));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<BehaviorTreeInstance>>(AIErrors::BehaviorTreeStorageUnavailable);
        }
    }

    /** @copydoc BehaviorTreeInstance::~BehaviorTreeInstance */
    BehaviorTreeInstance::~BehaviorTreeInstance() {
        Shutdown();
    }

    /** @copydoc BehaviorTreeInstance::Context */
    BehaviorTreeEvaluationContext BehaviorTreeInstance::Context(const DecisionNodeId node,
                                                                const BlackboardSnapshot &blackboard) const noexcept {
        return {node, plan_->Bindings().BindingsForNode(node), blackboard, tick_, reason_};
    }

    /** @copydoc BehaviorTreeInstance::Evaluate */
    Result<AiTaskState> BehaviorTreeInstance::Evaluate(const std::uint64_t tick, const BlackboardSnapshot &blackboard,
                                                       const AgentHandle activeAgent, const AiTaskResumeReason reason) {
        if (shutdown_ || evaluating_ || tick == 0 || tick < tick_ || reason >= AiTaskResumeReason::Count ||
            evaluation_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<AiTaskState>(AIErrors::TaskTransitionInvalid);
        EvaluationGuard guard(evaluating_);
        if (activeAgent != binding_.agent || !activeAgent.IsValid() || binding_.cancellation.IsCancellationRequested()) {
            CancelRange(0, activeAgent != binding_.agent ? AiTaskCancellationReason::AgentGenerationRetired
                                                         : AiTaskCancellationReason::ContextCancelled);
            if (states_[0].status == AiTaskState::Idle)
                states_[0].status = AiTaskState::Cancelled;
            shutdown_ = true;
            return Result<AiTaskState>::Success(states_[0].status);
        }
        if (blackboard.Binding() != binding_.blackboard)
            return Failure<AiTaskState>(AIErrors::BlackboardInstanceInvalid);
        if (const auto revision = blackboard.Revision(); revision.HasError())
            return Result<AiTaskState>::Failure(revision.ErrorValue());
        tick_ = tick;
        reason_ = reason;
        ++evaluation_;
        executor_->Pump();
        if (Terminal(states_[0].status))
            return Result<AiTaskState>::Success(states_[0].status);
        if (const auto observed = ObservePriority(blackboard); observed.HasError()) {
            CancelRange(0, AiTaskCancellationReason::Requested);
            return Result<AiTaskState>::Failure(observed.ErrorValue());
        }
        auto evaluated = Traverse(blackboard);
        if (evaluated.HasError())
            CancelRange(0, AiTaskCancellationReason::Requested);
        return evaluated;
    }

    /** @copydoc BehaviorTreeInstance::RunServices */
    Result<void> BehaviorTreeInstance::RunServices(const std::size_t node, const BlackboardSnapshot &blackboard) {
        const auto &record = plan_->Nodes()[node];
        const auto revision = blackboard.Revision();
        if (revision.HasError())
            return Result<void>::Failure(revision.ErrorValue());
        for (std::size_t index = record.firstService; index < record.firstService + record.serviceCount; ++index) {
            const auto &service = plan_->Services()[index];
            auto &state = services_[index];
            if (const bool due =
                    !state.active || (service.mode == BehaviorTreeServiceMode::Reactive ? state.revision != revision.Value()
                                                                                        : tick_ - state.tick >= service.intervalTicks);
                !due)
                continue;
            if (const auto result = executor_->Service(Context(service.id, blackboard)); result.HasError())
                return result;
            state = {.tick = tick_, .revision = revision.Value(), .active = true};
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::Check */
    Result<bool> BehaviorTreeInstance::Check(const std::size_t node, const BlackboardSnapshot &blackboard) {
        auto &state = states_[node];
        if (state.checkedEvaluation == evaluation_)
            return Result<bool>::Success(state.condition);
        const auto checked = executor_->Check(Context(plan_->Nodes()[node].execution.id, blackboard));
        if (checked.HasValue()) {
            state.condition = checked.Value();
            state.checkedEvaluation = evaluation_;
        }
        return checked;
    }

    /** @copydoc BehaviorTreeInstance::ObservePriority */
    Result<void> BehaviorTreeInstance::ObservePriority(const BlackboardSnapshot &blackboard) {
        for (std::size_t index = 0; index < plan_->Nodes().size(); ++index) {
            const auto &selector = plan_->Nodes()[index];
            auto &state = states_[index];
            if (selector.execution.operation != BehaviorTreeOperation::Selector || state.status != AiTaskState::Running)
                continue;
            for (std::size_t priority = 0; priority < state.cursor; ++priority) {
                const std::size_t child = plan_->Children()[selector.firstChild + priority];
                if (const auto mode = plan_->Nodes()[child].execution.abort;
                    mode != BehaviorTreeAbortMode::LowerPriority && mode != BehaviorTreeAbortMode::Both)
                    continue;
                const auto checked = Check(child, blackboard);
                if (checked.HasError())
                    return Result<void>::Failure(checked.ErrorValue());
                if (!checked.Value())
                    continue;
                const std::size_t active = plan_->Children()[selector.firstChild + state.cursor];
                CancelRange(active, AiTaskCancellationReason::Superseded);
                ResetRange(active);
                ResetRange(child);
                state.cursor = priority;
                break;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::Enter */
    Result<void> BehaviorTreeInstance::Enter(const std::size_t node, const BlackboardSnapshot &blackboard) {
        using enum AiTaskState;
        using enum BehaviorTreeOperation;
        const auto &record = plan_->Nodes()[node];
        auto &state = states_[node];
        const bool entering = state.status == Idle;
        if (entering) {
            state.status = Running;
            state.startedTick = tick_;
        }
        const auto op = record.execution.operation;
        if (op == Cooldown && entering && state.cooldownSet && tick_ - state.completedTick < record.execution.durationTicks)
            state.status = Failed;
        if (op == TimeLimit && tick_ - state.startedTick >= record.execution.durationTicks) {
            CancelRange(node, AiTaskCancellationReason::TimedOut);
            state.status = Failed;
        }
        if (op == BlackboardCheck &&
            (entering || record.execution.abort == BehaviorTreeAbortMode::Self || record.execution.abort == BehaviorTreeAbortMode::Both)) {
            const auto checked = Check(node, blackboard);
            if (checked.HasError())
                return Result<void>::Failure(checked.ErrorValue());
            if (!checked.Value()) {
                CancelRange(node, AiTaskCancellationReason::Requested);
                state.status = Failed;
            }
        }
        if (state.status == Running) {
            if (const auto serviced = RunServices(node, blackboard); serviced.HasError())
                return serviced;
            if (op == Task)
                return RunTask(node, blackboard);
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::RunTask */
    Result<void> BehaviorTreeInstance::RunTask(const std::size_t node, const BlackboardSnapshot &blackboard) {
        auto &state = states_[node];
        const bool starting = !state.task;
        if (starting) {
            if (const auto started = StartTask(node); started.HasError())
                return started;
        }
        const auto ready = state.task->PrepareResume(reason_, binding_.agent);
        if (ready.HasError())
            return Result<void>::Failure(ready.ErrorValue());
        if (ready.Value() != AiTaskResumeDisposition::Ready) {
            FinishTask(node);
            return Result<void>::Success();
        }
        if (auto pending = mailbox_->Take(node, state.task->Context()->task); pending.terminal) {
            ApplyTerminal(node, std::move(*pending.terminal));
        } else {
            auto context = Context(plan_->Nodes()[node].execution.id, blackboard);
            context.continuation = state.continuation;
            if (pending.event)
                context.reason = AiTaskResumeReason::Event;
            ApplyProviderResult(node, starting ? executor_->Start(context, *state.task->Context())
                                               : executor_->Resume(context, *state.task->Context()));
        }
        FinishTask(node);
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::FinishTask */
    void BehaviorTreeInstance::FinishTask(const std::size_t node) noexcept {
        auto &state = states_[node];
        state.status = state.task->State();
        if (!Terminal(state.status))
            return;
        mailbox_->Retire(node);
        if (const auto claim = state.task->ClaimCleanup(); !claim.HasValue() || !claim.Value())
            return;
        const auto &result = *state.task->TerminalResult();
        if (result.state == AiTaskState::Cancelled)
            executor_->Abort(*state.task->Context(), *result.cancellationReason);
        executor_->Cleanup(*state.task->Context(), result);
        static_cast<void>(state.task->CompleteCleanup());
    }

    /** @copydoc BehaviorTreeInstance::CancelRange */
    void BehaviorTreeInstance::CancelRange(const std::size_t node, const AiTaskCancellationReason reason) noexcept {
        for (std::size_t index = node; index < plan_->Nodes()[node].subtreeEnd; ++index) {
            auto &state = states_[index];
            if (state.status != AiTaskState::Running)
                continue;
            if (state.task) {
                static_cast<void>(state.task->RequestCancellation(reason));
                FinishTask(index);
            } else {
                state.status = AiTaskState::Cancelled;
            }
        }
    }

    /** @copydoc BehaviorTreeInstance::ResetRange */
    void BehaviorTreeInstance::ResetRange(const std::size_t node) const noexcept {
        for (std::size_t index = node; index < plan_->Nodes()[node].subtreeEnd; ++index) {
            auto &state = states_[index];
            state.status = AiTaskState::Idle;
            state.cursor = 0;
            state.iterations = 0;
            state.task.reset();
            state.continuation = {};
            // Cooldown and this evaluation's pure condition cache intentionally survive a branch restart.
            const auto &record = plan_->Nodes()[index];
            for (std::size_t service = record.firstService; service < record.firstService + record.serviceCount; ++service)
                services_[service] = {};
        }
    }

    /** @copydoc BehaviorTreeInstance::Propagate */
    void BehaviorTreeInstance::Propagate(const std::size_t parent, const std::size_t child) const noexcept {
        using enum AiTaskState;
        using enum BehaviorTreeOperation;
        auto &state = states_[parent];
        const auto &record = plan_->Nodes()[parent];
        const auto outcome = states_[child].status;
        const auto op = record.execution.operation;
        if (op == Parallel)
            return;
        if (outcome == Cancelled || outcome == Running) {
            state.status = outcome;
        } else if (op == Sequence || op == Selector) {
            if (outcome == (op == Sequence ? Succeeded : Failed)) {
                ++state.cursor;
                if (state.cursor < record.childCount)
                    return;
            }
            state.status = outcome;
        } else if (op == Inverter) {
            state.status = outcome == Succeeded ? Failed : Succeeded;
        } else if (op == Loop && outcome == Succeeded) {
            ++state.iterations;
            if (state.iterations < record.execution.iterations)
                ResetRange(child);
            else
                state.status = outcome;
        } else {
            state.status = outcome;
        }
        if (op == Cooldown && Terminal(outcome)) {
            state.completedTick = tick_;
            state.cooldownSet = true;
        }
    }

    /** @copydoc BehaviorTreeInstance::FinishParallel */
    void BehaviorTreeInstance::FinishParallel(const std::size_t nodeIndex) noexcept {
        using enum AiTaskState;
        const auto &node = plan_->Nodes()[nodeIndex];
        auto &state = states_[nodeIndex];
        std::size_t successes{};
        std::size_t failures{};
        std::size_t cancelled{};
        for (std::size_t child = node.firstChild; child < node.firstChild + node.childCount; ++child) {
            const auto outcome = states_[plan_->Children()[child]].status;
            if (outcome == Succeeded)
                ++successes;
            if (outcome == Failed)
                ++failures;
            if (outcome == Cancelled)
                ++cancelled;
        }
        const auto policy = node.execution.parallel;
        if (cancelled > 0)
            state.status = Cancelled;
        else if (policy == BehaviorTreeParallelPolicy::RequireOneSuccess && successes > 0)
            state.status = Succeeded;
        else if (policy == BehaviorTreeParallelPolicy::StopOthersOnFailure && failures > 0)
            state.status = Failed;
        else if (successes + failures == node.childCount)
            state.status = policy == BehaviorTreeParallelPolicy::RequireAllComplete || failures == 0 ? Succeeded : Failed;
        if (Terminal(state.status))
            CancelRange(nodeIndex, AiTaskCancellationReason::Requested);
    }

    /** @copydoc BehaviorTreeInstance::Traverse */
    Result<AiTaskState> BehaviorTreeInstance::Traverse(const BlackboardSnapshot &blackboard) {
        using enum BehaviorTreeOperation;
        std::size_t depth = 1;
        frames_[0] = {};
        std::size_t steps{};
        while (depth > 0) {
            ++steps;
            if (steps > plan_->Nodes().size() * 2)
                return Failure<AiTaskState>(AIErrors::BehaviorTreeLimitExceeded);
            auto &frame = frames_[depth - 1];
            const auto &node = plan_->Nodes()[frame.node];
            const auto &state = states_[frame.node];
            if (!frame.entered && !Terminal(state.status)) {
                if (const auto entered = Enter(frame.node, blackboard); entered.HasError())
                    return Result<AiTaskState>::Failure(entered.ErrorValue());
                frame.entered = true;
                frame.nextChild = node.execution.operation == Parallel ? 0 : state.cursor;
            }
            const bool parallel = node.execution.operation == Parallel;
            if (!Terminal(state.status) && frame.nextChild < node.childCount) {
                if (depth == plan_->Depth())
                    return Failure<AiTaskState>(AIErrors::BehaviorTreeLimitExceeded);
                const std::size_t child = plan_->Children()[node.firstChild + frame.nextChild++];
                frames_[depth++] = {.node = child};
                continue;
            }
            if (parallel && !Terminal(state.status)) {
                FinishParallel(frame.node);
            }
            const std::size_t completed = frame.node;
            --depth;
            if (depth == 0)
                break;
            auto &parentFrame = frames_[depth - 1];
            Propagate(parentFrame.node, completed);
            AdvanceFrame(parentFrame, completed);
        }
        return Result<AiTaskState>::Success(states_[0].status);
    }

    /** @copydoc BehaviorTreeInstance::AdvanceFrame */
    void BehaviorTreeInstance::AdvanceFrame(Frame &parent, const std::size_t completed) const noexcept {
        using enum BehaviorTreeOperation;
        const auto &node = plan_->Nodes()[parent.node];
        const auto op = node.execution.operation;
        if (op == Parallel)
            return;
        const bool composite = op == Sequence || op == Selector;
        if (composite ? states_[completed].status == AiTaskState::Running : states_[parent.node].status == AiTaskState::Running)
            parent.nextChild = node.childCount;
    }

    /** @copydoc BehaviorTreeInstance::Find */
    std::size_t BehaviorTreeInstance::Find(const DecisionNodeId node) const noexcept {
        for (std::size_t index = 0; index < plan_->Nodes().size(); ++index)
            if (plan_->Nodes()[index].execution.id == node)
                return index;
        return plan_->Nodes().size();
    }

    /** @copydoc BehaviorTreeInstance::AbortSubtree */
    Result<void> BehaviorTreeInstance::AbortSubtree(const DecisionNodeId node, const AiTaskCancellationReason reason) {
        if (evaluating_ || reason >= AiTaskCancellationReason::Count)
            return Failure<void>(AIErrors::TaskContextInvalid);
        const auto index = Find(node);
        if (index == plan_->Nodes().size())
            return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
        EvaluationGuard guard(evaluating_);
        CancelRange(index, reason);
        if (states_[index].status == AiTaskState::Idle)
            states_[index].status = AiTaskState::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::Restart */
    Result<void> BehaviorTreeInstance::Restart() {
        if (shutdown_ || evaluating_)
            return Failure<void>(AIErrors::TaskTransitionInvalid);
        EvaluationGuard guard(evaluating_);
        CancelRange(0, AiTaskCancellationReason::Superseded);
        ResetRange(0);
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::Replace */
    Result<bool> BehaviorTreeInstance::Replace(std::shared_ptr<const BehaviorTreeExecutionPlan> candidate) {
        if (!candidate || shutdown_ || evaluating_)
            return Failure<bool>(AIErrors::DecisionAssetActivationInvalid);
        if (candidate->Bindings().BlackboardSchema() != plan_->Bindings().BlackboardSchema())
            return Failure<bool>(AIErrors::DecisionAssetSchemaIncompatible);
        EvaluationGuard guard(evaluating_);
        if (plan_->IsCompatible(*candidate)) {
            plan_ = std::move(candidate);
            return Result<bool>::Success(true);
        }
        try {
            auto states = std::make_unique<NodeState[]>(candidate->Nodes().size());
            auto services = std::make_unique<ServiceState[]>(candidate->Services().size());
            auto frames = std::make_unique<Frame[]>(candidate->Depth());
            CancelRange(0, AiTaskCancellationReason::PlanReplaced);
            states_ = std::move(states);
            services_ = std::move(services);
            frames_ = std::move(frames);
            plan_ = std::move(candidate);
            return Result<bool>::Success(false);
        } catch (const std::bad_alloc &) {
            return Failure<bool>(AIErrors::BehaviorTreeStorageUnavailable);
        }
    }

    /** @copydoc BehaviorTreeInstance::Shutdown */
    void BehaviorTreeInstance::Shutdown() noexcept {
        if (shutdown_ || !plan_ || evaluating_)
            return;
        EvaluationGuard guard(evaluating_);
        CancelRange(0, AiTaskCancellationReason::OwnerShutdown);
        if (states_[0].status == AiTaskState::Idle)
            states_[0].status = AiTaskState::Cancelled;
        shutdown_ = true;
    }

    /** @copydoc BehaviorTreeInstance::Status */
    Result<AiTaskState> BehaviorTreeInstance::Status(const DecisionNodeId node) const {
        const auto index = Find(node);
        if (index == plan_->Nodes().size())
            return Failure<AiTaskState>(AIErrors::BehaviorTreeTopologyInvalid);
        return Result<AiTaskState>::Success(states_[index].status);
    }

    /** @copydoc BehaviorTreeInstance::RootStatus */
    AiTaskState BehaviorTreeInstance::RootStatus() const noexcept {
        return states_[0].status;
    }

    /** @copydoc BehaviorTreeInstance::TaskResult */
    Result<std::optional<AiTaskTerminalResult>> BehaviorTreeInstance::TaskResult(const DecisionNodeId node) const {
        const auto index = Find(node);
        if (index == plan_->Nodes().size() || plan_->Nodes()[index].execution.operation != BehaviorTreeOperation::Task)
            return Failure<std::optional<AiTaskTerminalResult>>(AIErrors::BehaviorTreeTopologyInvalid);
        const auto &task = states_[index].task;
        return Result<std::optional<AiTaskTerminalResult>>::Success(task && task->TerminalResult() ? std::optional{*task->TerminalResult()}
                                                                                                   : std::nullopt);
    }
}  // namespace Horo::AI
