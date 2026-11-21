#pragma once

/** @file AINavigationMovementTask.h
 * @brief Binds one exact AI task to the existing Navigation movement owner.
 */

#include "Horo/AI/AIRuntimeCapability.h"
#include "Horo/Navigation/NavigationMovement.h"

namespace Horo::AI {
    /** @brief Owner-lane binding between an admitted AI execution and one MoveTo or Follow operation.
     * @details The movement controller outlives this binding. The AI and Navigation owners remain authoritative
     * for their respective lifecycle state. Composing or advancing this binding never writes a Scene transform.
     */
    class AiNavigationMovementTask final {
    public:
        /** @brief Binds a running task and submits one typed movement request at intent dispatch.
         * @param ai Active Scene's AI owner. @param agent Exact AI agent. @param task Exact running execution.
         * @param movement Existing controller for the same live entity. @param request MoveTo or Follow request.
         * @param current Committed pre-movement observation. @return Bound operation or original admission error.
         */
        [[nodiscard]] static Result<AiNavigationMovementTask> Start(AiRuntimeCapability ai, AgentHandle agent, TaskHandle task,
                                                                    Navigation::NavigationMovementController &movement,
                                                                    Navigation::NavigationMovementRequest request,
                                                                    const Navigation::NavigationMovementObservation &current);

        /** @brief Cancels a still-active owned operation on the owner lane when the binding retires. */
        ~AiNavigationMovementTask();
        AiNavigationMovementTask(AiNavigationMovementTask &&other) noexcept;
        AiNavigationMovementTask(const AiNavigationMovementTask &) = delete;
        AiNavigationMovementTask &operator=(const AiNavigationMovementTask &) = delete;
        AiNavigationMovementTask &operator=(AiNavigationMovementTask &&) = delete;

        /** @brief Advances the current operation after Navigation query publication.
         * @param current Exact tick observation with optional previously achieved movement.
         * @return Real Navigation intent, empty after prior AI terminalization, or original typed movement failure.
         * @details The host passes the intent to its selected safe-velocity and Character path. The original
         * failure is also published to the exact AI execution. No provider work is awaited here.
         */
        [[nodiscard]] Result<std::optional<Navigation::NavigationMovementIntent>> Advance(
            const Navigation::NavigationMovementObservation &current);

        /** @brief Cancels the operation and its exact AI execution.
         * @return Original AI lifecycle transition or typed owner/identity failure.
         */
        [[nodiscard]] Result<AiTaskTransitionDisposition> Cancel();

    private:
        AiNavigationMovementTask(AiRuntimeCapability ai, AgentHandle agent, TaskHandle task,
                                 Navigation::NavigationMovementController &movement) noexcept;
        /** @brief Publishes original Navigation error as the canonical AI terminal cause. */
        [[nodiscard]] Error RecordFailure(AiTaskFailureKind kind, Error cause);
        /** @brief Projects terminal Navigation state into the existing AI task lifecycle. */
        [[nodiscard]] Result<void> ResolveOutcome();
        /** @brief Releases only the task's admitted movement and preserves any superseding operation. */
        bool CancelMotion() noexcept;

        AiRuntimeCapability ai_;
        AgentHandle agent_;
        TaskHandle task_;
        Navigation::NavigationMovementController *movement_{};
        Navigation::NavigationMovementId operation_;
        const std::thread::id owner_;
    };
}  // namespace Horo::AI
