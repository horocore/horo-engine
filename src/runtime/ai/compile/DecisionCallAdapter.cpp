#include "DecisionInstanceState.h"

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Unavailable() {
            return Result<T>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        }
    }  // namespace

    void DecisionGraphInstance::CallAdapter::Pump() noexcept {
        if (owner.Available())
            provider->Pump();
    }

    Result<AiTaskState> DecisionGraphInstance::CallAdapter::Start(const BehaviorTreeEvaluationContext &context,
                                                                  const AiTaskOperationContext &operation) noexcept {
        return owner.Available() ? provider->Start(context, operation) : Unavailable<AiTaskState>();
    }

    Result<AiTaskState> DecisionGraphInstance::CallAdapter::Resume(const BehaviorTreeEvaluationContext &context,
                                                                   const AiTaskOperationContext &operation) noexcept {
        return owner.Available() ? provider->Resume(context, operation) : Unavailable<AiTaskState>();
    }

    void DecisionGraphInstance::CallAdapter::Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason reason) noexcept {
        provider->Abort(operation, reason);
    }

    void DecisionGraphInstance::CallAdapter::Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept {
        provider->Cleanup(operation, result);
    }

    Result<bool> DecisionGraphInstance::CallAdapter::Check(const BehaviorTreeEvaluationContext &context) noexcept {
        return owner.Available() ? provider->Check(context) : Unavailable<bool>();
    }

    Result<void> DecisionGraphInstance::CallAdapter::Service(const BehaviorTreeEvaluationContext &context) noexcept {
        return owner.Available() ? provider->Service(context) : Unavailable<void>();
    }

    Result<AiTaskState> DecisionGraphInstance::CallAdapter::EvaluateSubplan(const BehaviorTreeEvaluationContext &context,
                                                                            bool entering) noexcept {
        return owner.Available() ? owner.Call(index, context.node, entering) : Unavailable<AiTaskState>();
    }

    void DecisionGraphInstance::CallAdapter::CancelSubplan(DecisionNodeId node, AiTaskCancellationReason reason) noexcept {
        owner.EndCall(index, node, reason);
    }
}  // namespace Horo::AI
