#pragma once
/** @file AIRuntimeCapability.h
 * @brief Revocable owner-thread AI access without a runtime pointer or backend service.
 */
#include "Horo/AI/AICapabilityDescriptors.h"
#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/BlackboardInstance.h"

#include <memory>
#include <thread>

namespace Horo::AI {
    class AiSceneRuntime;

    namespace Detail {
        struct AiSceneRuntimeState;
    }

    /** @brief Copied facts about one exact executing task. */
    struct AiCapabilityTaskSnapshot final {
        TaskHandle handle;
        TaskId definition;
        AiTaskState state{AiTaskState::Idle};
        std::optional<AiTaskTerminalResult> terminal;
        Telemetry::OperationContext diagnostics;
    };

    /**
     * @brief Scene-owner-issued access shared by native and script adapters.
     * @details Calls execute synchronously on the issuing simulation safe-point thread. The weak publication
     * never retains a scene. Replacement, shutdown, disabled agents, and cancelled grants fail closed.
     * Handles are process-local and must never be persisted. Blackboard writes use the existing atomic batch owner.
     */
    class AiRuntimeCapability final {
    public:
        AiRuntimeCapability() = default;
        /** @brief Checks the exact publication and simulation thread without looking up an agent.
         * @return Success or typed unavailable/revoked-owner failure. */
        [[nodiscard]] Result<void> ValidateAtSafePoint() const;
        /** @brief Resolves a durable ID in the exact bound publication. @param agent Authored identity. @return Exact handle or typed
         * failure. */
        [[nodiscard]] Result<AgentHandle> ResolveAgent(AgentId agent) const;
        /** @brief Starts an admitted definition. @param agent Exact agent. @param task Inert registered metadata. @param cancellation Grant
         * lifetime.
         * @param diagnostics Copied operation lineage. @return Exact task or typed failure. */
        [[nodiscard]] Result<TaskHandle> StartTask(AgentHandle agent, const AiCapabilityTaskDescriptor &task,
                                                   CancellationToken cancellation = {},
                                                   const Telemetry::OperationContext &diagnostics = {}) const;
        /** @brief Copies current task facts. @param agent Exact agent. @param task Exact execution. @return Owned facts or stale failure.
         */
        [[nodiscard]] Result<AiCapabilityTaskSnapshot> InspectTask(AgentHandle agent, TaskHandle task) const;
        /** @brief Cancels an exact execution idempotently. @param agent Exact agent. @param task Exact execution. @return Transition or
         * typed failure. */
        [[nodiscard]] Result<AiTaskTransitionDisposition> CancelTask(AgentHandle agent, TaskHandle task) const;
        /** @brief Publishes successful completion through the common lifecycle. @param agent Exact agent. @param task Exact execution.
         * @return Transition or typed failure; cancellation takes precedence. */
        [[nodiscard]] Result<AiTaskTransitionDisposition> CompleteTask(AgentHandle agent, TaskHandle task) const;
        /** @brief Releases terminal resources exactly once; running work must be completed or cancelled first.
         * @param agent Exact agent. @param task Exact execution. @return Success or typed lifecycle failure. */
        [[nodiscard]] Result<void> ReleaseTask(AgentHandle agent, TaskHandle task) const;
        /** @brief Reads one copied value without copying a whole blackboard. @param agent Exact agent. @param key Stable key. @return Value
         * or typed failure. */
        [[nodiscard]] Result<std::optional<BlackboardValue>> ReadBlackboard(AgentHandle agent, BlackboardKeyId key) const;
        /** @brief Creates a detached revision-fenced batch. @param agent Exact agent. @return Batch or typed failure. */
        [[nodiscard]] Result<BlackboardWriteBatch> BeginBlackboardWrite(AgentHandle agent) const;
        /** @brief Atomically commits a detached batch. @param agent Exact agent. @param batch Owned candidate. @return Changes or typed
         * failure. */
        [[nodiscard]] Result<BlackboardCommitResult> CommitBlackboardWrite(AgentHandle agent, BlackboardWriteBatch batch) const;

    private:
        friend class AiSceneRuntime;

        AiRuntimeCapability(std::weak_ptr<Detail::AiSceneRuntimeState> state, std::thread::id owner) noexcept
            : state_(std::move(state)), owner_(owner) {}

        /** @brief Checks issuing thread and exact publication before any mutable access. */
        [[nodiscard]] Result<std::shared_ptr<Detail::AiSceneRuntimeState>> Lock() const;
        std::weak_ptr<Detail::AiSceneRuntimeState> state_;
        std::thread::id owner_;
    };
}  // namespace Horo::AI
