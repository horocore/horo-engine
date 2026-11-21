#pragma once

/** @file HeadlessNavigationPathScope.h
 * @brief Private standalone Gameplay path grant over the installed immutable cooked provider.
 */
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

namespace Horo::Runtime {
    class RuntimeSceneView;
}

namespace Horo::Application::Internal {
    struct HeadlessNavigationPathPolicy;
    struct HeadlessNavigationActorPolicy;
    struct ProjectSceneNavigationSelection;
    struct ProjectSceneNavigationWorld;
    class ScenePhysicsHost;
    class SceneAiNavigationActor;

    /** @brief Owns the exact static world pin and module scope through terminal query retirement. */
    class HeadlessNavigationPathScope final {
    public:
        ~HeadlessNavigationPathScope();
        /** @brief Sets up one module's explicit registered filter against the already activated Scene world.
         * @param world Installed owner and its canonical coordinator. It outlives this scope.
         * @param selection The admitted cooked project policy.
         * @param policy Explicit filter/deadline settings. @param scene Exact private Scene incarnation.
         * @param module The one selected native principal for this standalone world.
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
        /** @brief Commits terminal results and refreshes the native scope on the fixed owner lane. */
        [[nodiscard]] Result<void> CommitAtFixedTick(std::uint64_t tick) const;
        /** @brief Dispatches only after gameplay has submitted from the same fixed owner lane. */
        void DispatchAtFixedTick(std::uint64_t tick) const;
        /** @brief Closes native admission before the Scene provider retires. */
        void Revoke() noexcept;
        /** @brief Polls existing coordinator fences without blocking or publishing late results. */
        [[nodiscard]] Result<bool> PollRetirement() const;

    private:
        HeadlessNavigationPathScope(std::shared_ptr<Navigation::NavigationWorldLifecycle> lifecycle,
                                    Navigation::NavigationCoordinator &coordinator,
                                    Navigation::NavigationWorldActivationDescriptor activation,
                                    Navigation::NavigationOutcomeProvenance source, Navigation::NavigationAgentRegistry &agents,
                                    std::vector<Navigation::NavigationPathCaller> callers,
                                    std::shared_ptr<Gameplay::GameplayNavigationContext> context) noexcept;

        std::shared_ptr<Navigation::NavigationWorldLifecycle> lifecycle_;
        Navigation::NavigationCoordinator &coordinator_;
        Navigation::NavigationWorldActivationDescriptor activation_;
        Navigation::NavigationOutcomeProvenance source_;
        Navigation::NavigationAgentRegistry &agents_;
        std::vector<Navigation::NavigationPathCaller> callers_;
        std::vector<std::unique_ptr<SceneAiNavigationActor>> actors_;
        std::shared_ptr<Gameplay::GameplayNavigationContext> context_;
    };
}  // namespace Horo::Application::Internal
