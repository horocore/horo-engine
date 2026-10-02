#pragma once

/**
 * @file AITaskLifecycle.h
 * @brief Generation-fenced lifecycle contract shared by native, script, and graph AI tasks.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <optional>

namespace Horo::AI {
    /** @brief Authoritative lifecycle state of one admitted AI task execution. */
    enum class AiTaskState : std::uint8_t {
        Idle,
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Typed origin of a task failure, independent of presentation text. */
    enum class AiTaskFailureKind : std::uint8_t {
        Admission,
        DependencyUnavailable,
        Execution,
        InvalidOutput,
        Count,
    };

    /** @brief Typed reason for a normal cancelled terminal outcome. */
    enum class AiTaskCancellationReason : std::uint8_t {
        Requested,
        ContextCancelled,
        AgentGenerationRetired,
        OwnerShutdown,
        TimedOut,
        Superseded,
        PlanReplaced,
        Count,
    };

    /** @brief Owner safe point at which a running task is eligible to resume. */
    enum class AiTaskResumeReason : std::uint8_t {
        FixedTick,
        Event,
        Count,
    };

    /** @brief Outcome of an idempotent lifecycle mutation. */
    enum class AiTaskTransitionDisposition : std::uint8_t {
        Applied,
        NoChange,
        AlreadyTerminal,
    };

    /** @brief Outcome of checking a tick or event resume boundary. */
    enum class AiTaskResumeDisposition : std::uint8_t {
        Ready,
        BecameTerminal,
        AlreadyTerminal,
    };

    /** @brief Typed failure payload retained by a failed terminal result. */
    struct AiTaskFailureDetail final {
        AiTaskFailureKind kind{AiTaskFailureKind::Execution}; /**< Stable failure family. */
        Error cause;                                          /**< Owned typed cause with actionable context. */
    };

    /**
     * @brief Immutable terminal outcome published exactly once for a started task.
     * @details Failure detail is present only for Failed; cancellation reason is present only for Cancelled.
     */
    struct AiTaskTerminalResult final {
        AiTaskState state{AiTaskState::Cancelled};                  /**< Exact terminal state. */
        std::optional<AiTaskFailureDetail> failure;                 /**< Owned detail for Failed. */
        std::optional<AiTaskCancellationReason> cancellationReason; /**< Typed reason for Cancelled. */
    };

    /**
     * @brief Immutable task inputs captured at admission without exposing mutable Scene state.
     * @details The task and agent handles must belong to one SceneRuntime incarnation. The cancellation token
     * is the only ambient lifecycle signal retained by the task contract.
     */
    struct AiTaskOperationContext final {
        TaskId taskDefinition;          /**< Persistent native, script, or graph task definition. */
        TaskHandle task;                /**< Exact process-local execution generation. */
        AgentHandle agent;              /**< Exact owning agent generation. */
        CancellationToken cancellation; /**< Cooperative cancellation tied to owner lifetime. */
    };

    /**
     * @brief Single-owner state machine for a cancellable AI task execution.
     * @details AIDecisionSystem owns mutation on its safe-point thread. Workers may publish detached outcomes
     * to that owner, but never mutate this object directly. Once terminal, later cancellation, stale completion,
     * or duplicate completion cannot replace the retained result. Cleanup may be claimed exactly once only after
     * terminal publication; the runner that owns this object also owns releasing downstream task resources.
     */
    class AiTaskLifecycle final {
    public:
        AiTaskLifecycle() = default;
        AiTaskLifecycle(const AiTaskLifecycle &) = delete;
        AiTaskLifecycle &operator=(const AiTaskLifecycle &) = delete;
        AiTaskLifecycle(AiTaskLifecycle &&) = delete;
        AiTaskLifecycle &operator=(AiTaskLifecycle &&) = delete;

        /**
         * @brief Admits one execution and captures its immutable operation context.
         * @param context Detached context for the exact task and agent generation.
         * @return Applied, or a typed context/transition failure. A pre-requested cancellation starts terminally Cancelled.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> Start(AiTaskOperationContext context);

        /**
         * @brief Checks cancellation and agent generation at a tick or event resume boundary.
         * @param reason Kind of owner safe point requesting task execution.
         * @param activeAgent Exact agent generation currently owned by the SceneRuntime.
         * @return Ready, BecameTerminal, AlreadyTerminal, or a typed invalid-input failure.
         */
        [[nodiscard]] Result<AiTaskResumeDisposition> PrepareResume(AiTaskResumeReason reason, AgentHandle activeAgent);

        /**
         * @brief Requests a normal cancelled outcome without replacing an existing terminal result.
         * @param reason Typed owner cancellation reason.
         * @return Mutation disposition or a typed invalid-reason failure.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> RequestCancellation(AiTaskCancellationReason reason);

        /**
         * @brief Retires the owning agent generation and fences all later completions.
         * @return Mutation disposition. A running execution becomes terminally Cancelled.
         */
        [[nodiscard]] AiTaskTransitionDisposition RetireAgentGeneration() noexcept;

        /**
         * @brief Publishes successful completion if cancellation and generation checks still permit it.
         * @param activeAgent Exact agent generation currently owned by the SceneRuntime.
         * @return Mutation disposition or a typed context/transition failure.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> CompleteSuccess(AgentHandle activeAgent);

        /**
         * @brief Publishes failed completion with an owned typed cause.
         * @param activeAgent Exact agent generation currently owned by the SceneRuntime.
         * @param failure Typed failure detail to retain.
         * @return Mutation disposition or a typed context/failure/transition error.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> CompleteFailure(AgentHandle activeAgent, AiTaskFailureDetail failure);

        /**
         * @brief Claims sole responsibility for cleanup after terminal publication.
         * @return True exactly once, false after another claim/completion, or a transition failure before terminal publication.
         */
        [[nodiscard]] Result<bool> ClaimCleanup();

        /**
         * @brief Records that the claiming runner released all downstream task resources.
         * @return Applied, NoChange when already completed, or a transition failure without a claim.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> CompleteCleanup();

        /** @brief Returns the current lifecycle state. @return Idle, Running, or the retained terminal state. */
        [[nodiscard]] AiTaskState State() const noexcept;
        /** @brief Returns the captured immutable context after start. @return Borrowed context, or null while Idle. */
        [[nodiscard]] const AiTaskOperationContext *Context() const noexcept;
        /** @brief Returns the immutable result after terminal publication. @return Borrowed result, or null before terminal state. */
        [[nodiscard]] const AiTaskTerminalResult *TerminalResult() const noexcept;
        /** @brief Reports whether cleanup has been acknowledged. @return True only after CompleteCleanup succeeds. */
        [[nodiscard]] bool IsCleanupComplete() const noexcept;

    private:
        /** @brief Internal exactly-once cleanup handoff state owned by the task runner. */
        enum class CleanupState : std::uint8_t {
            NotRequired,
            Pending,
            Claimed,
            Complete,
        };

        /** @brief Applies generation and cancellation precedence at one serialized owner boundary. */
        [[nodiscard]] Result<AiTaskResumeDisposition> CheckExecutionBoundary(AgentHandle activeAgent);
        /** @brief Applies the shared completion boundary and publishes success or a typed failure. */
        [[nodiscard]] Result<AiTaskTransitionDisposition> Complete(AgentHandle activeAgent, std::optional<AiTaskFailureDetail> failure);
        /** @brief Publishes the first cancelled terminal result after the caller proves the task is nonterminal. */
        void PublishCancelled(AiTaskCancellationReason reason) noexcept;

        AiTaskState state_{AiTaskState::Idle};
        std::optional<AiTaskOperationContext> context_;
        std::optional<AiTaskTerminalResult> terminalResult_;
        std::optional<AiTaskCancellationReason> pendingCancellation_;
        bool agentGenerationRetired_{false};
        CleanupState cleanupState_{CleanupState::NotRequired};
    };
}  // namespace Horo::AI
