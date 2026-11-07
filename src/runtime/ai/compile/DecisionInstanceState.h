#pragma once
#include "Horo/AI/DecisionGraphInstance.h"

namespace Horo::AI {
    struct DecisionGraphInstance::OccurrenceState final {
        std::unique_ptr<BehaviorTreeInstance> tree;
        std::unique_ptr<StateMachineInstance> machine;
        std::unique_ptr<ActionAdapter> actions;
        std::uint32_t generation{1};
        std::uint64_t lastTick{};
        AiTaskState outcome{AiTaskState::Idle};
        bool active{};
        bool started{};
        ~OccurrenceState();
    };

    struct DecisionGraphInstance::CallAdapter final : IBehaviorTreeExecutor {
        DecisionGraphInstance &owner;
        std::size_t index;
        std::unique_ptr<IBehaviorTreeExecutor> provider;

        CallAdapter(DecisionGraphInstance &instance, std::size_t occurrence, std::unique_ptr<IBehaviorTreeExecutor> callbacks)
            : owner(instance), index(occurrence), provider(std::move(callbacks)) {}

        void Pump() noexcept override;
        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &, const AiTaskOperationContext &) noexcept override;
        Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &, const AiTaskOperationContext &) noexcept override;
        void Abort(const AiTaskOperationContext &, AiTaskCancellationReason) noexcept override;
        void Cleanup(const AiTaskOperationContext &, const AiTaskTerminalResult &) noexcept override;
        Result<bool> Check(const BehaviorTreeEvaluationContext &) noexcept override;
        Result<void> Service(const BehaviorTreeEvaluationContext &) noexcept override;
        Result<AiTaskState> EvaluateSubplan(const BehaviorTreeEvaluationContext &, bool entering) noexcept override;
        void CancelSubplan(DecisionNodeId, AiTaskCancellationReason) noexcept override;

        [[nodiscard]] bool SupportsSubplans() const noexcept override {
            return true;
        }
    };

    struct DecisionGraphInstance::ActionAdapter final : StateMachineActions {
        struct Action final {
            DecisionNodeId node;
            std::uint32_t slot{};
            std::uint32_t generation{1};
            AiTaskLifecycle lifecycle;
        };

        DecisionGraphInstance &owner;
        std::size_t index;
        std::unique_ptr<IBehaviorTreeExecutor> provider;
        std::vector<std::unique_ptr<Action>> actions;

        ActionAdapter(DecisionGraphInstance &instance, std::size_t occurrence, std::unique_ptr<IBehaviorTreeExecutor> callbacks)
            : owner(instance), index(occurrence), provider(std::move(callbacks)) {}

        Result<void> Prepare();
        Result<void> Invoke(const StateMachineActionInvocation &) noexcept override;
        Result<void> EnterState(const StateMachineStateInvocation &) noexcept override;
        Result<AiTaskState> UpdateState(const StateMachineStateInvocation &) noexcept override;
        Result<void> ExitState(const StateMachineStateInvocation &) noexcept override;
        void CancelState(DecisionNodeId, AiTaskCancellationReason) noexcept override;

        [[nodiscard]] bool SupportsSubplans() const noexcept override {
            return true;
        }

        Result<void> Complete(Action &action, Result<AiTaskState> result);
    };
}  // namespace Horo::AI
