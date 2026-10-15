#include "Horo/Gameplay/GameplayAiContext.h"

#include <algorithm>
#include <new>

namespace Horo::Gameplay {
    /** @copydoc GameplayAiContext::GameplayAiContext */
    GameplayAiContext::GameplayAiContext(const CreationKey &, GameplayAiBinding binding, AI::AiRuntimeCapability capability,
                                         CancellationSource cancellation, AI::AiCapabilityDescriptorSet declarations)
        : binding_(std::move(binding)), capability_(std::move(capability)), cancellation_(std::move(cancellation)),
          declarations_(std::move(declarations)) {}

    /** @copydoc GameplayAiContext::Create */
    Result<std::shared_ptr<GameplayAiContext>> GameplayAiContext::Create(const GameplayAiBinding &binding,
                                                                         AI::AiRuntimeCapability capability,
                                                                         AI::AiCapabilityDescriptorSet declarations,
                                                                         CancellationToken cancellation) {
        if (binding.moduleId.empty() || !binding.moduleEnabled || !binding.permissionGranted)
            return Result<std::shared_ptr<GameplayAiContext>>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        if (auto valid = capability.ValidateAtSafePoint(); valid.HasError())
            return Result<std::shared_ptr<GameplayAiContext>>::Failure(valid.ErrorValue());
        try {
            const CreationKey key;
            return Result<std::shared_ptr<GameplayAiContext>>::Success(
                std::make_shared<GameplayAiContext>(key, binding, std::move(capability), CancellationSource{cancellation},
                                                    std::move(declarations)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayAiContext>>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        }
    }

    GameplayAiContext::~GameplayAiContext() {
        Revoke();
    }

    /** @copydoc GameplayAiContext::Revoke */
    void GameplayAiContext::Revoke() const noexcept {
        cancellation_.RequestCancellation();
    }

    /** @copydoc GameplayAiContext::RetireOwnedTasksAtSafePoint */
    void GameplayAiContext::RetireOwnedTasksAtSafePoint() const {
        if (std::this_thread::get_id() != owner_)
            return;
        for (auto &owned : tasks_) {
            if (!owned.task.IsValid())
                continue;
            try {
                if (auto cancelled = capability_.CancelTask(owned.agent, owned.task); cancelled.HasError())
                    continue;
                auto released = capability_.ReleaseTask(owned.agent, owned.task);
                if (released.HasError())
                    continue;
            } catch (const std::bad_alloc &) {
                continue;
            }
            owned = {};
        }
    }

    /** @copydoc GameplayAiContext::Check */
    Result<void> GameplayAiContext::Check() const {
        if (std::this_thread::get_id() != owner_ || !binding_.moduleEnabled || !binding_.permissionGranted ||
            cancellation_.Token().IsCancellationRequested())
            return Result<void>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc GameplayAiContext::Derive */
    Result<std::shared_ptr<GameplayAiContext>> GameplayAiContext::Derive(AI::AiCapabilityDescriptorSet declarations) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<std::shared_ptr<GameplayAiContext>>::Failure(admitted.ErrorValue());
        try {
            const CreationKey key;
            return Result<std::shared_ptr<GameplayAiContext>>::Success(
                std::make_shared<GameplayAiContext>(key, binding_, capability_, CancellationSource{cancellation_.Token()},
                                                    std::move(declarations)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayAiContext>>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc GameplayAiContext::ResolveAgent */
    Result<AI::AgentHandle> GameplayAiContext::ResolveAgent(AI::AgentId agent) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::AgentHandle>::Failure(admitted.ErrorValue());
        return capability_.ResolveAgent(agent);
    }

    /** @copydoc GameplayAiContext::StartTask */
    Result<AI::TaskHandle> GameplayAiContext::StartTask(AI::AgentHandle agent, AI::TaskId task,
                                                        const Telemetry::OperationContext &diagnostics) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::TaskHandle>::Failure(admitted.ErrorValue());
        const auto descriptor = declarations_.Find(task);
        if (!descriptor)
            return Result<AI::TaskHandle>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        auto vacant = std::ranges::find_if(tasks_, [](const OwnedTask &slot) {
            return !slot.task.IsValid();
        });
        if (vacant == tasks_.end())
            return Result<AI::TaskHandle>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        auto started = capability_.StartTask(agent, *descriptor, cancellation_.Token(), diagnostics);
        if (started.HasValue())
            *vacant = {agent, started.Value()};
        return started;
    }

    /** @copydoc GameplayAiContext::Owns */
    bool GameplayAiContext::Owns(AI::AgentHandle agent, AI::TaskHandle task) const noexcept {
        return std::ranges::any_of(tasks_, [&](const OwnedTask &owned) {
            return owned.agent == agent && owned.task == task && task.IsValid();
        });
    }

    /** @copydoc GameplayAiContext::InspectTask */
    Result<AI::AiCapabilityTaskSnapshot> GameplayAiContext::InspectTask(AI::AgentHandle agent, AI::TaskHandle task) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::AiCapabilityTaskSnapshot>::Failure(admitted.ErrorValue());
        if (!Owns(agent, task))
            return Result<AI::AiCapabilityTaskSnapshot>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        return capability_.InspectTask(agent, task);
    }

    /** @copydoc GameplayAiContext::CancelTask */
    Result<AI::AiTaskTransitionDisposition> GameplayAiContext::CancelTask(AI::AgentHandle agent, AI::TaskHandle task) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::AiTaskTransitionDisposition>::Failure(admitted.ErrorValue());
        if (!Owns(agent, task))
            return Result<AI::AiTaskTransitionDisposition>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        return capability_.CancelTask(agent, task);
    }

    /** @copydoc GameplayAiContext::CompleteTask */
    Result<AI::AiTaskTransitionDisposition> GameplayAiContext::CompleteTask(AI::AgentHandle agent, AI::TaskHandle task) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::AiTaskTransitionDisposition>::Failure(admitted.ErrorValue());
        if (!Owns(agent, task))
            return Result<AI::AiTaskTransitionDisposition>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        return capability_.CompleteTask(agent, task);
    }

    /** @copydoc GameplayAiContext::ReleaseTask */
    Result<void> GameplayAiContext::ReleaseTask(AI::AgentHandle agent, AI::TaskHandle task) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        if (!Owns(agent, task))
            return Result<void>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        auto released = capability_.ReleaseTask(agent, task);
        if (released.HasValue())
            for (auto &owned : tasks_)
                if (owned.agent == agent && owned.task == task)
                    owned = {};
        return released;
    }

    /** @copydoc GameplayAiContext::ReadBlackboard */
    Result<std::optional<AI::BlackboardValue>> GameplayAiContext::ReadBlackboard(AI::AgentHandle agent, AI::BlackboardKeyId key) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<std::optional<AI::BlackboardValue>>::Failure(admitted.ErrorValue());
        return capability_.ReadBlackboard(agent, key);
    }

    /** @copydoc GameplayAiContext::BeginBlackboardWrite */
    Result<AI::BlackboardWriteBatch> GameplayAiContext::BeginBlackboardWrite(AI::AgentHandle agent) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::BlackboardWriteBatch>::Failure(admitted.ErrorValue());
        return capability_.BeginBlackboardWrite(agent);
    }

    /** @copydoc GameplayAiContext::CommitBlackboardWrite */
    Result<AI::BlackboardCommitResult> GameplayAiContext::CommitBlackboardWrite(AI::AgentHandle agent,
                                                                                AI::BlackboardWriteBatch batch) const {
        if (auto admitted = Check(); admitted.HasError())
            return Result<AI::BlackboardCommitResult>::Failure(admitted.ErrorValue());
        return capability_.CommitBlackboardWrite(agent, std::move(batch));
    }

    /** @copydoc AcquireGameplayAi */
    Result<std::shared_ptr<const GameplayAiContext>> AcquireGameplayAi(const GameRuntimeContext &context) {
        if (!context.ai || context.cancellation.IsCancellationRequested())
            return Result<std::shared_ptr<const GameplayAiContext>>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        if (auto admitted = context.ai->Check(); admitted.HasError())
            return Result<std::shared_ptr<const GameplayAiContext>>::Failure(admitted.ErrorValue());
        return Result<std::shared_ptr<const GameplayAiContext>>::Success(context.ai);
    }

}  // namespace Horo::Gameplay
