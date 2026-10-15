#pragma once
/** @file GameplayAiContext.h
 * @brief Module-scoped AI access shared by native gameplay and script invocation adapters.
 */
#include "Horo/AI/AIRuntimeCapability.h"
#include "Horo/Gameplay/GameModule.h"

#include <memory>
#include <string>

namespace Horo::Gameplay {
    class GameModuleHost;
    class LoadedGameModule;

    /** @brief Explicit module admission; no global runtime discovery or backend selection. */
    struct GameplayAiBinding final {
        std::string moduleId;
        bool moduleEnabled{};
        bool permissionGranted{};
    };

    /**
     * @brief Host-owned revocable principal and copied task declarations.
     * @details All operations except Revoke run synchronously on the AI simulation safe-point thread.
     * The host retains this holder independently from its exposed context field, revoking before module callbacks/unload.
     * Native and script paths use these exact operations and the existing AI scene/task authority.
     */
    class GameplayAiContext final {
    public:
        /** @brief Factory-only token; admission cannot be bypassed by a consumer. */
        class CreationKey final {
            CreationKey() = default;
            friend class GameplayAiContext;
        };

        /** @brief Constructs already validated state. @param key Private factory authority. @param binding Copied policy.
         * @param capability Weak exact publication. @param cancellation Owned module revocation. @param declarations Frozen inert tasks. */
        GameplayAiContext(const CreationKey &key, GameplayAiBinding binding, AI::AiRuntimeCapability capability,
                          CancellationSource cancellation, AI::AiCapabilityDescriptorSet declarations);
        /** @brief Admits an explicit module scope. @param binding Host-selected principal and permission.
         * @param capability Owner-issued scene access. @param declarations Frozen inert task declarations. @param cancellation Parent
         * lifetime. @return Scoped holder or typed policy/storage failure. */
        [[nodiscard]] static Result<std::shared_ptr<GameplayAiContext>> Create(const GameplayAiBinding &binding,
                                                                               AI::AiRuntimeCapability capability,
                                                                               AI::AiCapabilityDescriptorSet declarations = {},
                                                                               CancellationToken cancellation = {});
        ~GameplayAiContext();
        GameplayAiContext(const GameplayAiContext &) = delete;
        GameplayAiContext &operator=(const GameplayAiContext &) = delete;
        /** @brief Permanently revokes admission and running tasks; safe from any thread. */
        void Revoke() const noexcept;

        /** @brief Returns copied immutable host policy. @return Bound principal and permission. */
        [[nodiscard]] const GameplayAiBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Resolves a stable authored agent ID. @param agent Stable identity. @return Exact handle or typed missing/revoked failure.
         */
        [[nodiscard]] Result<AI::AgentHandle> ResolveAgent(AI::AgentId agent) const;
        /** @brief Starts a registered task under this module lifetime. @param agent Exact agent. @param task Registered identity.
         * @param diagnostics Copied operation lineage. @return Exact task execution or typed failure. */
        [[nodiscard]] Result<AI::TaskHandle> StartTask(AI::AgentHandle agent, AI::TaskId task,
                                                       const Telemetry::OperationContext &diagnostics) const;
        /** @brief Starts without diagnostic storage or a caller-side owning default context.
         * @param agent Exact agent. @param task Registered identity. @return Exact task execution or typed failure. */
        [[nodiscard]] Result<AI::TaskHandle> StartTask(AI::AgentHandle agent, AI::TaskId task) const;
        /** @brief Copies exact task facts. @param agent Exact agent. @param task Exact task. @return Owned facts or typed failure. */
        [[nodiscard]] Result<AI::AiCapabilityTaskSnapshot> InspectTask(AI::AgentHandle agent, AI::TaskHandle task) const;
        /** @brief Cancels exact work. @param agent Exact agent. @param task Exact task. @return Shared lifecycle transition or failure. */
        [[nodiscard]] Result<AI::AiTaskTransitionDisposition> CancelTask(AI::AgentHandle agent, AI::TaskHandle task) const;
        /** @brief Completes exact work. @param agent Exact agent. @param task Exact task. @return Shared lifecycle transition or failure.
         */
        [[nodiscard]] Result<AI::AiTaskTransitionDisposition> CompleteTask(AI::AgentHandle agent, AI::TaskHandle task) const;
        /** @brief Releases terminal execution resources. @param agent Exact agent. @param task Exact task. @return Success or typed
         * failure. */
        [[nodiscard]] Result<void> ReleaseTask(AI::AgentHandle agent, AI::TaskHandle task) const;
        /** @brief Reads an authorized owned value. @param agent Exact agent. @param key Stable key. @return Copied value or failure. */
        [[nodiscard]] Result<std::optional<AI::BlackboardValue>> ReadBlackboard(AI::AgentHandle agent, AI::BlackboardKeyId key) const;
        /** @brief Starts an atomic blackboard transaction. @param agent Exact agent. @return Detached batch or failure. */
        [[nodiscard]] Result<AI::BlackboardWriteBatch> BeginBlackboardWrite(AI::AgentHandle agent) const;
        /** @brief Commits through BlackboardSync authority. @param agent Exact agent. @param batch Owned writes. @return Changes or
         * failure. */
        [[nodiscard]] Result<AI::BlackboardCommitResult> CommitBlackboardWrite(AI::AgentHandle agent, AI::BlackboardWriteBatch batch) const;

    private:
        /** @brief Shares admission while distinguishing absent diagnostics from an explicit captured context. */
        [[nodiscard]] Result<AI::TaskHandle> StartTaskCaptured(AI::AgentHandle agent, AI::TaskId task,
                                                               const Telemetry::OperationContext *diagnostics) const;
        friend class GameModuleHost;
        friend class LoadedGameModule;
        friend Result<std::shared_ptr<const GameplayAiContext>> AcquireGameplayAi(const GameRuntimeContext &context);
        /** @brief Cancels and releases module executions on the simulation owner before callbacks/unload. */
        void RetireOwnedTasksAtSafePoint() const;
        /** @brief Creates an independent child module lifetime with frozen registration metadata. */
        [[nodiscard]] Result<std::shared_ptr<GameplayAiContext>> Derive(AI::AiCapabilityDescriptorSet declarations) const;
        /** @brief Rejects revoked or withheld scopes before any runtime operation. */
        [[nodiscard]] Result<void> Check() const;
        /** @brief Enforces exact execution ownership independently of scene-wide handle validity. */
        [[nodiscard]] bool Owns(AI::AgentHandle agent, AI::TaskHandle task) const noexcept;

        struct OwnedTask final {
            AI::AgentHandle agent;
            AI::TaskHandle task;
        };

        mutable std::array<OwnedTask, AI::MaximumAiCapabilityTasks> tasks_{};
        const std::thread::id owner_{std::this_thread::get_id()};
        GameplayAiBinding binding_;
        AI::AiRuntimeCapability capability_;
        CancellationSource cancellation_;
        AI::AiCapabilityDescriptorSet declarations_;
    };

    /** @brief Acquires only the host-supplied optional runtime AI grant; absence is a recoverable typed error.
     * @param context Explicit module generation context. @return Immutable scoped holder or typed missing/revoked capability. */
    [[nodiscard]] Result<std::shared_ptr<const GameplayAiContext>> AcquireGameplayAi(const GameRuntimeContext &context);
}  // namespace Horo::Gameplay
