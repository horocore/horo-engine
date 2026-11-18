#pragma once

#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationSceneActivation.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Navigation {
    class NavigationDefinition;
}

namespace Horo::Application::Internal {
    /** @brief Synchronous borrow of the admitted workspace, committed cook, and exact selected Scene identity. */
    struct ProjectSceneNavigationSource final {
        const Platform::IProjectReadFiles &files;
        const Platform::ProjectReadContext &context;
        const Assets::AssetRegistrySnapshot &registry;
        const Runtime::RuntimeSceneDefinition &definition;
        Assets::AssetId scene;
        AssetCookTargetId target;
        JobSystem &jobs;
    };

    /** @brief Canonical owner-thread policy captured once for the chosen durable Scene. */
    struct ProjectSceneNavigationSelection final {
        Navigation::NavigationBakeSurfaceKey surface;
        Navigation::NavigationAgentProfileId profile;
        AssetCookTargetId target;
        Navigation::NavigationProjectProfile policy;
        std::shared_ptr<const Navigation::NavigationDefinition> catalog;
        JobSystem &jobs;
    };

    /** @brief Explicit lifetime owners for one authoritative world, retained until its Scene participants shut down. */
    struct ProjectSceneNavigationWorld final {
        std::unique_ptr<Assets::AssetPayloadCache> cache;
        Navigation::NavigationAgentRegistry agents;
        Navigation::NavigationCoordinator coordinator;
    };

    /** @brief Captures the registered definition, selected scoped surface and exact project runtime policy. */
    [[nodiscard]] Result<std::shared_ptr<const ProjectSceneNavigationSelection>> CaptureProjectSceneNavigation(
        const ProjectSceneNavigationSource &workspace);

    /** @brief Reserves one distinct world-lifetime population and coordinator from the captured project limits. */
    [[nodiscard]] Result<std::unique_ptr<ProjectSceneNavigationWorld>> CreateProjectSceneNavigationWorld(
        const ProjectSceneNavigationSelection &selection);

    /** @brief Registers the provider, initial catalog and atomic prefab agent participant on the actual Scene service. */
    [[nodiscard]] Result<void> RegisterProjectSceneNavigation(Runtime::RuntimeSceneService &scene, ProjectSceneNavigationWorld &world,
                                                              std::shared_ptr<const ProjectSceneNavigationSelection> selection);
}  // namespace Horo::Application::Internal
