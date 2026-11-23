#pragma once

/** @file SceneAiNavigationMotion.h
 * @brief Application-owned simulation phases for one exact AI navigation and Character binding.
 */

#include "Horo/AI/AINavigationMovementTask.h"
#include "Horo/Navigation/NavigationCharacterAdapter.h"

namespace Horo::Application::Internal {
    class ScenePhysicsHost;

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
            const Navigation::NavigationAvoidanceRequest &request, Navigation::INavigationCrowdBackend &solver);

        /** @brief Prepares an owned solver request on the Scene lane without submitting Character motion.
         * @param current Committed pre-Character observation. @param capture Exact owned crowd snapshot.
         * @param request Selected avoidance envelope.
         * @return Work for the solver, empty for a retired task, or the typed admission failure.
         */
        [[nodiscard]] Result<std::optional<Navigation::NavigationVelocityWork>> Prepare(
            const Navigation::NavigationMovementObservation &current, const Navigation::NavigationCrowdSnapshot &capture,
            const Navigation::NavigationAvoidanceRequest &request);

        /** @brief Consumes the exact prepared result on the Scene lane before the Character step.
         * @param work Owner-issued work returned by Prepare. @param solved Outcome or solver failure.
         * @param current Fresh pre-Character observation. @param capture Exact same-tick snapshot.
         * @return Character-admitted desired motion, or the original typed failure.
         */
        [[nodiscard]] Result<Navigation::NavigationVelocityPublication> Complete(const Navigation::NavigationVelocityWork &work,
                                                                                 Result<Navigation::NavigationAvoidanceOutcome> solved,
                                                                                 const Navigation::NavigationMovementObservation &current,
                                                                                 const Navigation::NavigationCrowdSnapshot &capture);

        /** @brief Returns achieved motion after Character publishes the admitted tick.
         * @param tick Committed fixed tick. @return Success or the original stale or Character failure.
         */
        [[nodiscard]] Result<void> Observe(std::uint64_t tick);
        /** @brief Retains a failed motion observation or publication on the exact AI task. */
        void Reject(Error error);
        /** @brief Discards feedback for a failed or abandoned Character attempt before retirement. */
        void DiscardFeedback() noexcept;

    private:
        /** @brief Validates one solved intent and admits it to Character without stepping simulation. */
        [[nodiscard]] Result<Navigation::NavigationVelocityPublication> Publish(const Navigation::NavigationVelocityWork &work,
                                                                                const Navigation::NavigationAvoidanceOutcome &solved,
                                                                                const Navigation::NavigationMovementObservation &current,
                                                                                const Navigation::NavigationCrowdSnapshot &capture);
        /** @brief Releases the exact prepared velocity work during error or owner retirement. */
        void DiscardPrepared() noexcept;

        AI::AiNavigationMovementTask task_;
        Navigation::NavigationVelocityPipeline &velocity_;
        Navigation::NavigationCharacterAdapter &character_;
        std::optional<Navigation::NavigationVelocityWork> prepared_;
        std::uint64_t pendingTick_{};
    };

    /** @brief Prepared per-actor Navigation, Character and task ownership for a configured Scene. */
    class SceneAiNavigationActor final {
    public:
        /** @brief Resolves the registered Navigation agent and dedicated Character controller for the same live entity.
         * @param registry Committed Navigation registrations. @param world Active Navigation provider owner.
         * @param coordinator Existing path job and publication owner. @param caller Explicit world-local path caller.
         * @param physics Published native Scene aggregate. @param scene Current committed Scene view.
         * @param entity Exact actor generation. @param principal Host-selected Character producer.
         * @return Actor ownership or original typed admission failure. Borrowed owners outlive the actor.
         */
        [[nodiscard]] static Result<std::unique_ptr<SceneAiNavigationActor>> CreateForScene(
            Navigation::NavigationAgentRegistry &registry, Navigation::NavigationWorldLifecycle &world,
            Navigation::NavigationCoordinator &coordinator, Navigation::NavigationPathCaller caller, ScenePhysicsHost &physics,
            const Runtime::RuntimeSceneView &scene, Runtime::EntityRef entity, CharacterInput::DesiredMotionPrincipal principal);

        /** @brief Prepares the host-selected Navigation and Character owners before starting AI work.
         * @param registry Committed Navigation registrations. @param world Active Navigation provider owner.
         * @param coordinator Existing path job and publication owner. @param caller Explicit world-local path caller.
         * @param agent Exact registered agent. @param character Dedicated Character grant. @param binding Exact Character owner facts.
         * @return Actor ownership or original typed admission failure. Borrowed owners outlive the actor.
         */
        [[nodiscard]] static Result<std::unique_ptr<SceneAiNavigationActor>> Create(
            Navigation::NavigationAgentRegistry &registry, Navigation::NavigationWorldLifecycle &world,
            Navigation::NavigationCoordinator &coordinator, Navigation::NavigationPathCaller caller, Navigation::CrowdAgentHandle agent,
            Character::CharacterCapability character, const Navigation::NavigationCharacterBinding &binding);

        /** @brief Starts the staged graph task at intent dispatch. */
        [[nodiscard]] Result<void> Admit(AI::AiRuntimeCapability ai, AI::AiTaskContinuation task,
                                         Navigation::NavigationMovementRequest request,
                                         const Navigation::NavigationMovementObservation &current);
        /** @brief Borrows the active motion for this actor until the next admission or retirement. */
        [[nodiscard]] SceneAiNavigationMotion *Motion() noexcept;
        /** @brief Replaces the observation's root with the committed Character position. */
        [[nodiscard]] Result<Navigation::NavigationMovementObservation> Capture(Navigation::NavigationMovementObservation current) const;
        /** @brief Copies the actor's committed root and achieved velocity for the crowd capture. */
        [[nodiscard]] Result<Navigation::NavigationCrowdMotionSample> Sample(std::uint64_t tick) const;
        /** @brief Returns stable registration identity used to locate this actor in an immutable crowd capture. */
        [[nodiscard]] Navigation::CrowdAgentHandle Agent() const noexcept;

    private:
        SceneAiNavigationActor(std::unique_ptr<Navigation::NavigationMovementController> movement,
                               std::unique_ptr<Navigation::NavigationVelocityPipeline> velocity,
                               Navigation::NavigationCharacterAdapter bridge, Character::CharacterCapability character,
                               Navigation::NavigationCharacterBinding binding, Navigation::CrowdAgentHandle agent) noexcept;

        std::unique_ptr<Navigation::NavigationMovementController> movement_;
        std::unique_ptr<Navigation::NavigationVelocityPipeline> velocity_;
        Navigation::NavigationCharacterAdapter bridge_;
        Character::CharacterCapability character_;
        Navigation::NavigationCharacterBinding binding_;
        Navigation::CrowdAgentHandle agent_;
        AI::AiTaskContinuation task_;
        std::optional<SceneAiNavigationMotion> motion_;
    };
}  // namespace Horo::Application::Internal
