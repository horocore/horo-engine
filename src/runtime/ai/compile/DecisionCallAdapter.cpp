#include "DecisionInstanceState.h"

#include <new>

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
        if (!owner.Available())
            return Unavailable<AiTaskState>();
        auto result = provider->Start(context, operation);
        ObserveTaskFailure(context.node, result);
        return result;
    }

    Result<AiTaskState> DecisionGraphInstance::CallAdapter::Resume(const BehaviorTreeEvaluationContext &context,
                                                                   const AiTaskOperationContext &operation) noexcept {
        if (!owner.Available())
            return Unavailable<AiTaskState>();
        auto result = provider->Resume(context, operation);
        ObserveTaskFailure(context.node, result);
        return result;
    }

    /** @copydoc DecisionGraphInstance::CallAdapter::ObserveTaskFailure */
    void DecisionGraphInstance::CallAdapter::ObserveTaskFailure(DecisionNodeId node, const Result<AiTaskState> &result) const noexcept {
        if (!result.HasError() || !owner.input_ || !owner.Available())
            return;
        auto *failure = owner.input_->providerFailure;
        if (!failure || *failure)
            return;
        try {
            failure->emplace(DecisionProviderFailure{owner.TraceOccurrence(index).graph, node, result.ErrorValue()});
        } catch (const std::bad_alloc &) {
            // Evidence must not change the task outcome when diagnostic memory is unavailable.
        }
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
