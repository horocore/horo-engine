#include "AITaskMailbox.h"
#include "BehaviorTreeInstanceState.h"

#include <limits>
#include <utility>

namespace Horo::AI {
    /** @copydoc BehaviorTreeInstance::StartTask */
    Result<void> BehaviorTreeInstance::StartTask(const std::size_t node) {
        if (nextGeneration_ == 0)
            return Result<void>::Failure(MakeError(AIErrors::GenerationExhausted));
        auto &state = states_[node];
        const TaskHandle handle{binding_.agent.incarnation, {binding_.firstTaskSlot + static_cast<std::uint32_t>(node), nextGeneration_}};
        nextGeneration_ = nextGeneration_ == std::numeric_limits<std::uint32_t>::max() ? 0 : nextGeneration_ + 1;
        state.task.emplace();
        if (const auto started = state.task->Start({plan_->Nodes()[node].execution.task, handle, binding_.agent, binding_.cancellation});
            started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        if (auto continuation = mailbox_->Bind(node, *state.task->Context()); continuation.HasError()) {
            static_cast<void>(state.task->CompleteFailure(binding_.agent, {AiTaskFailureKind::Admission, continuation.ErrorValue()}));
        } else {
            state.continuation = std::move(continuation).Value();
        }
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeInstance::ApplyTerminal */
    void BehaviorTreeInstance::ApplyTerminal(const std::size_t node, AiTaskTerminalResult result) const {
        auto &task = *states_[node].task;
        if (result.state == AiTaskState::Succeeded)
            static_cast<void>(task.CompleteSuccess(binding_.agent));
        else if (result.state == AiTaskState::Failed)
            static_cast<void>(task.CompleteFailure(binding_.agent, std::move(*result.failure)));
        else
            static_cast<void>(task.RequestCancellation(*result.cancellationReason));
    }

    /** @copydoc BehaviorTreeInstance::ApplyProviderResult */
    void BehaviorTreeInstance::ApplyProviderResult(const std::size_t node, const Result<AiTaskState> &result) const {
        using enum AiTaskState;
        using enum AiTaskFailureKind;
        if (result.HasError()) {
            ApplyTerminal(node, {.state = Failed, .failure = AiTaskFailureDetail{Execution, result.ErrorValue()}});
        } else if (result.Value() == Succeeded) {
            ApplyTerminal(node, {.state = Succeeded});
        } else if (result.Value() == Cancelled) {
            ApplyTerminal(node, {.state = Cancelled, .cancellationReason = AiTaskCancellationReason::Requested});
        } else if (result.Value() != Running) {
            const auto kind = result.Value() == Failed ? Execution : InvalidOutput;
            ApplyTerminal(node, {.state = Failed, .failure = AiTaskFailureDetail{kind, MakeError(AIErrors::TaskFailureInvalid)}});
        }
    }

}  // namespace Horo::AI
