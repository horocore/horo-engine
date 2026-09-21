#pragma once

/**
 * @file NavigationSceneActivation.h
 * @brief Runtime-scene activation participant for provider-neutral navigation agents.
 */

#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <memory>

namespace Horo::Navigation {
    /**
     * @brief Prepares and atomically publishes the logical navigation-agent population for one runtime Scene.
     * @details The participant owns no provider-native state. It translates exact runtime entity generations into a detached
     * agent-registry candidate and publishes that candidate only after the aggregate RuntimeScene validates every participant.
     */
    class NavigationSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        /**
         * @brief Binds a registry whose lifetime exceeds this participant and every returned candidate.
         * @param registry Owner-thread logical agent registry.
         */
        explicit NavigationSceneActivationParticipant(NavigationAgentRegistry &registry) noexcept : registry_(&registry) {}

        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         Runtime::RuntimeSceneView scene) override;

    private:
        NavigationAgentRegistry *registry_{};
    };
}  // namespace Horo::Navigation
