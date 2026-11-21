#pragma once

/** @file HeadlessNavigationPathScope.h
 * @brief Private authoritative Gameplay path grant over the installed immutable cooked provider.
 */
#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Navigation/NavigationCrowdSnapshot.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

namespace Horo::Runtime {
    class RuntimeSceneView;
    class SceneAnimationRuntime;
}  // namespace Horo::Runtime

namespace Horo::AI {
    class AiRuntimeCapability;
    class AiTaskContinuation;
}  // namespace Horo::AI

namespace Horo::Application::Internal {
    struct HeadlessNavigationPathPolicy;
    struct HeadlessNavigationActorPolicy;
    struct HeadlessNavigationMotionPolicy;
    struct ProjectSceneNavigationSelection;
    struct ProjectSceneNavigationWorld;
    class ScenePhysicsHost;
    class SceneAiNavigationActor;

    /** @brief Portable terminal result evidence with no process-local handles. */
    struct HeadlessReplayNavigationResult final {
        std::uint64_t sequence{};
        std::optional<Navigation::NavigationPathStatus> status;
        std::optional<Navigation::NavigationPathStopReason> stopReason;
        std::size_t points{};
        Sha256Digest digest;
    };

    /** @brief One bounded best-effort Navigation publication captured for diagnostic use. */
    struct HeadlessReplayNavigation final {
        static constexpr std::size_t MaximumResults = 32;
        Sha256Digest profile;
        std::uint64_t tick{};
        std::uint64_t topology{};
        std::array<HeadlessReplayNavigationResult, MaximumResults> results{};
        std::size_t count{};
        bool truncated{};
    };

    /** @brief Owns the exact static world pin and module scope through terminal query retirement. */
    class HeadlessNavigationPathScope final {
    public:
        ~HeadlessNavigationPathScope();
        /** @brief Sets up one module's explicit registered filter against the already activated Scene world.
         * @param world Installed owner and its canonical coordinator. It outlives this scope.
         * @param selection The admitted cooked project policy.
         * @param policy Explicit filter/deadline settings. @param scene Exact private Scene incarnation.
         * @param module The one selected native principal for this authoritative world.
         * @return Prepared scope with initial observation, or original typed admission error.
         */
        [[nodiscard]] static Result<std::unique_ptr<HeadlessNavigationPathScope>> Create(ProjectSceneNavigationWorld &world,
                                                                                         const ProjectSceneNavigationSelection &selection,
                                                                                         const HeadlessNavigationPathPolicy &policy,
                                                                                         Runtime::SceneRuntimeId scene,
                                                                                         std::string_view module);

        /** @brief Borrows the only module scope bound to the exact published static world. */
        [[nodiscard]] const std::shared_ptr<Gameplay::GameplayNavigationContext> &Context() const noexcept;
        /** @brief Prepares the selected actors and registers distinct callers before the first fixed tick. */
        [[nodiscard]] Result<void> PrepareActors(ScenePhysicsHost &physics, const Runtime::RuntimeSceneView &scene,
                                                 std::span<const HeadlessNavigationActorPolicy> selected);
        /** @brief Opens the exact fixed-tick observation used by staged and active motion. */
        void BeginMotionTick(std::uint64_t tick, float stepSeconds) noexcept;
        /** @brief Resolves the exact AI task and its prepared Scene actor at decision intent dispatch. */
        [[nodiscard]] Result<void> AdmitMotion(const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy,
                                               AI::AiRuntimeCapability ai, AI::AiTaskContinuation task,
                                               Navigation::NavigationMovementRequest request, std::uint64_t tick,
                                               Runtime::SceneAnimationRuntime *animation);
        /** @brief Submits safe Navigation motion before the native Character step. */
        [[nodiscard]] Result<void> SubmitMotion(const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy);
        /** @brief Retains achieved motion after native publication, or discards an aborted attempt. */
        void ObserveMotion(std::uint64_t tick);
        void DiscardMotionFeedback() noexcept;
        /** @brief Commits terminal results and refreshes the native scope on the fixed owner lane. */
        [[nodiscard]] Result<void> CommitAtFixedTick(std::uint64_t tick, HeadlessReplayNavigation *replay = nullptr) const;
        /** @brief Dispatches only after gameplay has submitted from the same fixed owner lane. */
        void DispatchAtFixedTick(std::uint64_t tick) const;
        /** @brief Closes native admission before the Scene provider retires. */
        void Revoke() noexcept;
        /** @brief Polls existing coordinator fences without blocking or publishing late results. */
        [[nodiscard]] Result<bool> PollRetirement() const;

    private:
        struct Actor final {
            std::unique_ptr<SceneAiNavigationActor> owner;
            std::optional<Navigation::NavigationAgentOwner> target;
            std::optional<Animation::GraphParameterId> motionStartTrigger;
        };

        [[nodiscard]] Result<Navigation::NavigationMovementObservation> ObserveActor(
            const Runtime::RuntimeSceneView &scene, const HeadlessNavigationMotionPolicy &policy, const SceneAiNavigationActor &actor,
            std::optional<Navigation::NavigationAgentOwner> target) const;
        HeadlessNavigationPathScope(std::shared_ptr<Navigation::NavigationWorldLifecycle> lifecycle,
                                    Navigation::NavigationCoordinator &coordinator,
                                    Navigation::NavigationWorldActivationDescriptor activation,
                                    Navigation::NavigationOutcomeProvenance source, Navigation::NavigationAgentRegistry &agents,
                                    std::vector<Navigation::NavigationPathCaller> callers,
                                    std::shared_ptr<Gameplay::GameplayNavigationContext> context, Sha256Digest replayProfile) noexcept;

        std::shared_ptr<Navigation::NavigationWorldLifecycle> lifecycle_;
        Navigation::NavigationCoordinator &coordinator_;
        Navigation::NavigationWorldActivationDescriptor activation_;
        Navigation::NavigationOutcomeProvenance source_;
        Navigation::NavigationAgentRegistry &agents_;
        std::vector<Navigation::NavigationPathCaller> callers_;
        std::vector<Actor> actors_;
        std::vector<Navigation::NavigationCrowdMotionSample> samples_;
        std::uint64_t motionTick_{};
        float motionStep_{};
        std::shared_ptr<Gameplay::GameplayNavigationContext> context_;
        Sha256Digest replayProfile_;
    };
}  // namespace Horo::Application::Internal
