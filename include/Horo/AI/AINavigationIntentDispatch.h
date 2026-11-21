#pragma once

/** @file AINavigationIntentDispatch.h
 * @brief Bounded owner-lane handoff from cooked AI evaluation to Navigation intent dispatch.
 */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/AITaskContinuation.h"
#include "Horo/Navigation/NavigationMovement.h"

namespace Horo::AI {
    /** @brief Stores at most one staged movement request per agent for the canonical decision commit phase. */
    class AiNavigationIntentDispatch final : public IAiCookedDecisionIntentSink {
    public:
        /** @brief Host admission of exact tree work. The host retains the admitted movement until it terminates. */
        using Admit = std::function<Result<void>(const AiTaskContinuation &, Navigation::NavigationMovementRequest, std::uint64_t)>;

        /** @brief Allocate dispatch slots outside the fixed loop.
         * @param agents Exact upper bound for the Scene agent registry.
         * @param admit Host-selected Navigation admission with captures valid for this dispatcher's lifetime.
         * @return Owned dispatcher or typed capacity/policy error.
         */
        [[nodiscard]] static Result<std::shared_ptr<AiNavigationIntentDispatch>> Create(std::size_t agents, Admit admit);

        /** @brief Stage a typed request in the current decision evaluation.
         * @param task Exact continuation captured by the behavior-tree task. @param tick Evaluation tick.
         * @param request Owned MoveTo or Follow request, carried unchanged into admission.
         * @return Success or original typed queue admission error.
         */
        [[nodiscard]] Result<void> Stage(AiTaskContinuation task, std::uint64_t tick, Navigation::NavigationMovementRequest request);

        /** @copydoc IAiCookedDecisionIntentSink::Commit */
        [[nodiscard]] Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override;
        /** @copydoc IAiCookedDecisionIntentSink::Discard */
        void Discard(AgentHandle agent) noexcept override;

    private:
        struct Entry final {
            AiTaskContinuation task;
            Navigation::NavigationMovementRequest request;
            std::uint64_t tick{};
            std::optional<AiTaskTerminalResult> rejected;
        };

        AiNavigationIntentDispatch(std::size_t agents, Admit admit);
        /** @brief Calls the selected host admission once and offers failures to the original task. */
        [[nodiscard]] Result<bool> Publish(Entry &entry);

        std::vector<Entry> entries_;
        Admit admit_;
        std::thread::id owner_;
    };
}  // namespace Horo::AI
