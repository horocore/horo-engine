#pragma once

#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationSceneActivation.h"

namespace Horo::Navigation {
    class NavigationDefinition;
}

namespace Horo::Application::Internal {
    struct HeadlessProjectSceneOptions;
    struct HeadlessProjectSceneResources;

    /** @brief Canonical owner-thread policy captured once for the chosen durable Scene. */
    struct HeadlessNavigationSelection final {
        Navigation::NavigationBakeSurfaceKey surface;
        Navigation::NavigationAgentProfileId profile;
        AssetCookTargetId target;
        Navigation::NavigationProjectProfile policy;
        std::shared_ptr<const Navigation::NavigationDefinition> catalog;
        JobSystem &jobs;
    };

    /** @brief Explicit lifetime owners for one authoritative world, retained until its Scene participants shut down. */
    struct HeadlessNavigationWorld final {
        std::unique_ptr<Assets::AssetPayloadCache> cache;
        Navigation::NavigationAgentRegistry agents;
        Navigation::NavigationCoordinator coordinator;
        Navigation::NavigationAssetSceneActivationParticipant *assets{}; /**< Borrowed from the later-destroyed Scene service. */
    };

    /** @brief Captures the registered definition, selected scoped surface and exact project runtime policy. */
    [[nodiscard]] Result<std::shared_ptr<const HeadlessNavigationSelection>> CaptureHeadlessNavigation(
        const HeadlessProjectSceneOptions &options, HeadlessProjectSceneResources &resources);

    /** @brief Reserves one distinct world-lifetime population and coordinator from the captured project limits. */
    [[nodiscard]] Result<std::unique_ptr<HeadlessNavigationWorld>> CreateHeadlessNavigationWorld(
        const HeadlessNavigationSelection &selection);

    /** @brief Registers the provider, initial catalog and atomic prefab agent participant on the actual Scene service. */
    [[nodiscard]] Result<void> RegisterHeadlessNavigation(Runtime::RuntimeSceneService &scene, HeadlessNavigationWorld &world,
                                                          std::shared_ptr<const HeadlessNavigationSelection> selection);
}  // namespace Horo::Application::Internal
