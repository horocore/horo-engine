#include "Horo/AI/AINavigationIntentDispatch.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <new>

namespace Horo::AI {
    AiNavigationIntentDispatch::AiNavigationIntentDispatch(const std::size_t agents, Admit admit)
        : entries_(agents), admit_(std::move(admit)), owner_(std::this_thread::get_id()) {}

    /** @copydoc AiNavigationIntentDispatch::Create */
    Result<std::shared_ptr<AiNavigationIntentDispatch>> AiNavigationIntentDispatch::Create(const std::size_t agents, Admit admit) {
        using Created = Result<std::shared_ptr<AiNavigationIntentDispatch>>;
        if (agents == 0 || agents > MaximumAiSceneAgents || !admit)
            return Created::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        try {
            return Created::Success(std::shared_ptr<AiNavigationIntentDispatch>(new AiNavigationIntentDispatch(agents, std::move(admit))));
        } catch (const std::bad_alloc &) {
            return Created::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiNavigationIntentDispatch::Stage */
    Result<void> AiNavigationIntentDispatch::Stage(AiTaskContinuation task, const std::uint64_t tick,
                                                   Navigation::NavigationMovementRequest request) {
        const auto agent = task.Operation().agent;
        if (owner_ != std::this_thread::get_id() || tick == 0 || agent.slot.index >= entries_.size() || !task.IsCurrent())
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto &entry = entries_[agent.slot.index];
        if (entry.task.IsCurrent())
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::AdmissionRejected));
        entry = {std::move(task), std::move(request), tick, std::nullopt};
        return Result<void>::Success();
    }

    /** @copydoc AiNavigationIntentDispatch::Commit */
    Result<bool> AiNavigationIntentDispatch::Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept {
        if (owner_ != std::this_thread::get_id() || context.agent.slot.index >= entries_.size())
            return Result<bool>::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto &entry = entries_[context.agent.slot.index];
        if (entry.task.Operation().agent != context.agent)
            return Result<bool>::Success(true);
        if (!entry.task.IsCurrent() || context.cancellation.IsCancellationRequested()) {
            entry = {};
            return Result<bool>::Success(true);
        }
        if (entry.tick != context.tick)
            return Result<bool>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (!budget.Consume())
            return Result<bool>::Success(false);
        try {
            return Publish(entry);
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiNavigationIntentDispatch::Publish */
    Result<bool> AiNavigationIntentDispatch::Publish(Entry &entry) {
        if (!entry.rejected) {
            auto admitted = admit_(entry.task, std::move(entry.request), entry.tick);
            if (admitted.HasValue()) {
                entry = {};
                return Result<bool>::Success(true);
            }
            entry.rejected.emplace(
                AiTaskTerminalResult{.state = AiTaskState::Failed,
                                     .failure = AiTaskFailureDetail{AiTaskFailureKind::Admission, std::move(admitted).ErrorValue()}});
        }
        const auto offered = entry.task.PublishTerminal(*entry.rejected);
        if (offered.HasError())
            return Result<bool>::Failure(offered.ErrorValue());
        if (offered.Value() == AiTaskPublicationDisposition::Contended)
            return Result<bool>::Success(false);
        entry = {};
        return Result<bool>::Success(true);
    }

    /** @copydoc AiNavigationIntentDispatch::Discard */
    void AiNavigationIntentDispatch::Discard(const AgentHandle agent) noexcept {
        if (owner_ == std::this_thread::get_id() && agent.slot.index < entries_.size() &&
            entries_[agent.slot.index].task.Operation().agent == agent)
            entries_[agent.slot.index] = {};
    }
}  // namespace Horo::AI
