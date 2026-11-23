#pragma once

#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationRuntimeRebuildJobs.h"
#include "Horo/Navigation/NavigationSceneActivation.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Navigation {
    class NavigationDefinition;
}

namespace Horo::Application::Internal {
    class MountedWorld;

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
        Assets::AssetId definition;
        Navigation::NavigationAgentProfileId profile;
        AssetCookTargetId target;
        Navigation::NavigationProjectProfile policy;
        std::shared_ptr<const Navigation::NavigationDefinition> catalog;
        JobSystem &jobs;
        std::uint64_t surfaceGeneration{};
    };

    /** @brief Host-authorized immutable geometry change and live authority required at the Scene cutoff. */
    struct ProjectSceneNavigationRebuildCommand final {
        Navigation::NavigationRuntimeRebuildFence expected;
        Navigation::NavigationGeneration nextTopology;
        std::shared_ptr<const Navigation::NavigationBakeInputSnapshot> input;
        Navigation::NavigationTileBakeCompatibility compatibility;
        Navigation::NavigationRuntimeRebuildAuthority observe;
        std::size_t maximumRebuiltTiles{};
        std::size_t maximumOwnedBytes{};
        CancellationToken cancellation;
    };

    /** @brief Explicit lifetime owners for one authoritative world, retained until its Scene participants shut down. */
    struct ProjectSceneNavigationWorld final {
        std::unique_ptr<Assets::AssetPayloadCache> cache;
        Navigation::NavigationAgentRegistry agents;
        Navigation::NavigationCoordinator coordinator;
        std::uint32_t maximumPendingPathRequests{};                      /**< Exact coordinator caller reservation capacity. */
        Navigation::NavigationAssetSceneActivationParticipant *assets{}; /**< Borrowed until Scene service teardown. */
        bool streamCoverage{};                                           /**< The bound Scene starts without queryable tile coverage. */
        std::unique_ptr<Navigation::NavigationRuntimeRebuildJobs> rebuild;
        Navigation::NavigationRuntimeRebuildAuthority rebuildAuthority;
        CancellationToken rebuildCancellation;
    };

    /** @brief Captures the registered definition, selected scoped surface and exact project runtime policy. */
    [[nodiscard]] Result<std::shared_ptr<const ProjectSceneNavigationSelection>> CaptureProjectSceneNavigation(
        const ProjectSceneNavigationSource &workspace);

    /** @brief Builds a query-only provider from the exact scoped cook without creating a world or caller reservations. */
    [[nodiscard]] Result<std::unique_ptr<Navigation::INavigationQueryBackend>> PrepareProjectNavigationPreviewBackend(
        const ProjectSceneNavigationSelection &selection, const Navigation::NavMeshAssetSource &assets,
        const Navigation::NavigationWorldActivationDescriptor &identity, const CancellationToken &cancellation);

    /** @brief Reserves one distinct world-lifetime population and coordinator from the captured project limits.
     * @param selection Exact project scope and captured runtime limits.
     * @param execution Host-selected path mode for the composed Recast/Detour world.
     * @return World-lifetime owners or a typed preparation error.
     */
    [[nodiscard]] Result<std::unique_ptr<ProjectSceneNavigationWorld>> CreateProjectSceneNavigationWorld(
        const ProjectSceneNavigationSelection &selection,
        Navigation::NavigationPathExecution execution = Navigation::NavigationPathExecution::BestEffortJobs);

    /** @brief Registers the provider, initial catalog and atomic prefab agent participant on the actual Scene service. */
    [[nodiscard]] Result<void> RegisterProjectSceneNavigation(Runtime::RuntimeSceneService &scene, ProjectSceneNavigationWorld &world,
                                                              std::shared_ptr<const ProjectSceneNavigationSelection> selection,
                                                              bool streamCoverage = false, bool runtimeRebuild = false);

    /** @brief Submits one authorized runtime command using only the active Scene's canonical tile layout and budget. */
    [[nodiscard]] Result<void> QueueProjectSceneNavigationRebuild(ProjectSceneNavigationWorld &world,
                                                                  ProjectSceneNavigationRebuildCommand command);

    /** @brief Publishes a ready rebuild on the owner lane after Scene lifecycle commit and before fixed work/readers.
     * @param world Exact registered project Scene owner.
     * @param cancellation Current frame or shutdown cancellation.
     * @return True for a new published topology, false if no result is ready, or the typed rejected-command error.
     */
    [[nodiscard]] Result<bool> CommitProjectSceneNavigationRebuild(ProjectSceneNavigationWorld &world,
                                                                   const CancellationToken &cancellation);

    /** @brief Binds the selected native provider to live committed World cells after the root is active. */
    [[nodiscard]] Result<void> BindMountedWorldNavigation(MountedWorld &mount, ProjectSceneNavigationWorld &world,
                                                          std::shared_ptr<const ProjectSceneNavigationSelection> selection);
}  // namespace Horo::Application::Internal
