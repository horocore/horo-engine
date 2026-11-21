#pragma once

/** @file SceneAiNavigationMotion.h
 * @brief Application-owned simulation phases for one exact AI navigation and Character binding.
 */

#include "Horo/AI/AINavigationMovementTask.h"
#include "Horo/Navigation/NavigationCharacterAdapter.h"

namespace Horo::Application::Internal {
    /** @brief Owns an admitted task and borrows the exact Character bridge and velocity pipeline on the Scene owner lane. */
    class SceneAiNavigationMotion final {
    public:
        /** @brief Stores the same actor's prepared authorities, all of which outlive this task.
         * @param task Exact admitted execution. @param velocity Safe-velocity owner. @param character Dedicated Character bridge.
         */
        SceneAiNavigationMotion(AI::AiNavigationMovementTask task, Navigation::NavigationVelocityPipeline &velocity,
                                Navigation::NavigationCharacterAdapter &character) noexcept;
        ~SceneAiNavigationMotion();
        SceneAiNavigationMotion(const SceneAiNavigationMotion &) = delete;
        SceneAiNavigationMotion &operator=(const SceneAiNavigationMotion &) = delete;

        /** @brief Admits safe desired motion immediately before Character for this fixed tick.
         * @param current Committed pre-Character observation. @param capture Complete same-tick crowd evidence.
         * @param request Selected avoidance envelope. @param solver Host-selected bounded avoidance provider.
         * @return The admitted publication, empty when the task has retired, or the typed failure retained on the task.
         */
        [[nodiscard]] Result<std::optional<Navigation::NavigationVelocityPublication>> Submit(
            const Navigation::NavigationMovementObservation &current, const Navigation::NavigationCrowdSnapshot &capture,
            Navigation::NavigationAvoidanceRequest request, Navigation::INavigationCrowdBackend &solver);

        /** @brief Returns achieved motion after Character publishes the admitted tick.
         * @param tick Committed fixed tick. @return Success or the original stale or Character failure.
         */
        [[nodiscard]] Result<void> Observe(std::uint64_t tick);
        /** @brief Discards feedback for a failed or abandoned Character attempt before retirement. */
        void DiscardFeedback() noexcept;

    private:
        /** @brief Solves, validates and submits exactly one work item, without stepping native simulation. */
        [[nodiscard]] Result<Navigation::NavigationVelocityPublication> SubmitWork(const Navigation::NavigationVelocityWork &work,
                                                                                   const Navigation::NavigationMovementObservation &current,
                                                                                   const Navigation::NavigationCrowdSnapshot &capture,
                                                                                   Navigation::INavigationCrowdBackend &solver);

        AI::AiNavigationMovementTask task_;
        Navigation::NavigationVelocityPipeline &velocity_;
        Navigation::NavigationCharacterAdapter &character_;
        std::uint64_t pendingTick_{};
    };
}  // namespace Horo::Application::Internal
