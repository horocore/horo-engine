#pragma once

/**
 * @file NavigationSceneActivation.h
 * @brief Runtime-scene activation participant for provider-neutral navigation agents.
 */

#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <functional>
#include <memory>
#include <utility>

namespace Horo::Navigation {
    class NavigationWorldLifecycle;
    class NavigationCoordinator;

    /**
     * @brief Prepares and atomically publishes the logical navigation-agent population for one runtime Scene.
     * @details The participant owns no provider-native state. It translates exact runtime entity generations into a detached
     * agent-registry candidate and publishes that candidate only after the aggregate RuntimeScene validates every participant.
     */
    class NavigationSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        /** @brief Validates the authored agent using the configured host's profile, filter and surface authority. */
        using StructuralAdmission = std::function<Result<void>(const NavigationAgentDescriptor &)>;
        /** @brief Pins the current active world once during structural preparation. */
        using ActiveWorldSource = std::function<std::shared_ptr<NavigationWorldLifecycle>()>;

        /**
         * @brief Binds a registry whose lifetime exceeds this participant and every returned candidate.
         * @param registry Owner-thread logical agent registry.
         * @param initialAdmission Optional catalog validation for initial agents.
         */
        explicit NavigationSceneActivationParticipant(NavigationAgentRegistry &registry, StructuralAdmission initialAdmission = {}) noexcept
            : registry_(&registry), initialAdmission_(std::move(initialAdmission)) {}

        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         const Runtime::RuntimeSceneView &scene) override;

        /** @brief Composes atomic logical agent changes for this registry on an exact active provider world.
         * @param world Host-owned Navigation world outliving the participant and its candidates.
         * @param coordinator Host-owned coordinator for this world, outliving the participant and its candidates.
         * @param admission Catalog validation for additions. A missing validator rejects addition explicitly.
         * @return Participant to attach to the same RuntimeSceneService as this activation participant.
         * @details Stages only provider-neutral registrations. No Character authority, positions, topology or active movement
         * are synthesized. Retirement abandons owned path reservations and drops exact logical handles before callbacks. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(NavigationWorldLifecycle &world,
                                                                                                     NavigationCoordinator &coordinator,
                                                                                                     StructuralAdmission admission) const;
        /** @brief Composes structural admission against the asset participant's currently published world.
         * @param world Retained owner-thread resolver for the real asset lifecycle.
         * @param coordinator Host-owned coordinator outliving the participant and its candidates.
         * @param admission Exact catalog policy for additions.
         * @return Participant to attach to the same Scene service. An absent active world refuses additions. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(ActiveWorldSource world,
                                                                                                     NavigationCoordinator &coordinator,
                                                                                                     StructuralAdmission admission) const;

    private:
        NavigationAgentRegistry *registry_{};
        StructuralAdmission initialAdmission_;
    };
}  // namespace Horo::Navigation
