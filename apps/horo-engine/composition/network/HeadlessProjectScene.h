#pragma once
#include "Horo/Prefab/PrefabMetrics.h"

/** @file HeadlessProjectScene.h
 * @brief Application-owned project Scene inputs and immutable load resources for one explicit mode host.
 */
#include "CommittedSceneAssets.h"
#include "HeadlessNetworkServices.h"
#include "HeadlessSimulationAssets.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Scene/SceneSource.h"
#include "MountedWorldHost.h"
#include "ProjectScenePerception.h"

namespace Horo::Application::Internal {
    enum class HeadlessNavigationMode : std::uint8_t {
        Disabled,
        RecastDetour
    };

    /** @brief Explicit Scene input parsed at the CLI boundary, without backend discovery. */
    struct HeadlessProjectSceneOptions final {
        std::filesystem::path projectRoot;
        std::string scene;
        std::filesystem::path cookedRoot;
        std::string cookTarget;
        Physics::PhysicsRuntimeMode physicsMode{Physics::PhysicsRuntimeMode::Null};
        std::chrono::milliseconds loadTimeout{30'000};
        std::string publicationNamespace; /**< Empty preserves read-only hosts; explicit derived output selected by the application. */
        HeadlessNavigationMode navigationMode{HeadlessNavigationMode::Disabled};
        std::optional<CommittedWorldSelection> world; /**< Explicit independent committed World selection. */
        std::string perceptionPolicyPath;             /**< Explicit project-relative source for one selected committed Scene. */
        bool captureNetworkSettings{};                /**< Authenticated network gameplay selects the existing project settings. */
        std::size_t cpuWorkerCount{1};                /**< Explicit process CPU worker allocation selected before preparation. */
        std::optional<HeadlessSimulationAssets> simulationAssets; /**< Explicit dedicated payload retention policy. */
    };

    /** @brief Owns all borrowed assets/jobs/project captures until every world service has retired.
     * @details Its operation boundary may be replaced only before an entire owner-lane Startup/Travel call.
     */
    struct HeadlessProjectSceneResources final {
        explicit HeadlessProjectSceneResources(std::size_t cpuWorkerCount) : jobs(JobSystemConfig{cpuWorkerCount, 128}) {}

        JobSystem jobs;
        Assets::AssetRegistry registry;
        std::unique_ptr<CommittedSceneAssets> committed;
        Assets::MemoryAssetProvider empty;
        std::unique_ptr<Assets::AssetLoadService> loads;
        std::unique_ptr<Assets::AssetLoadService> prefabLoads;
        std::shared_ptr<const ProjectCollisionSettings> collision;
        CancellationSource cancellation;
        std::chrono::steady_clock::time_point deadline;
        HeadlessSceneLoadBoundary load;
        std::shared_ptr<const Platform::IProjectReadFiles> sourceFiles;
        std::shared_ptr<DurableFileSystem> publication;
        Platform::ProjectReadContext sourceContext;
        std::optional<SceneSource::SceneSourceDocument> authoredSource; /**< Exact decoded admitted source, never a second parse. */
        std::optional<Network::NetworkProjectSettings> networkSettings; /**< Owner-validated snapshot captured from the same root. */
        AI::AiTaskSchedulerSettings decisionScheduler;                  /**< Authoritative scheduler budgets from that same snapshot. */
        bool developmentReplayAllowed{true}; /**< Legacy projects retain the explicit development CLI workflow. */
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition;
        Assets::AssetId sceneAsset; /**< Exact selected cooked Scene identity, including the full UUID. */
        std::shared_ptr<const ProjectSceneNavigationSelection> navigation;
        std::shared_ptr<const ProjectScenePerceptionPolicy>
            perceptionPolicy; /**< Matched source and definition, without provider activation. */
        std::optional<Physics::PhysicsSceneActivationSettings> settings;
        Prefab::PrefabMetricHandles prefabMetrics; /**< Process-prebound numeric observations retained by host resources. */
        Prefab::PrefabProjectPolicy prefabPolicy;  /**< Project-launch capture, passed by value to source-free world composition. */
    };

    /** @brief Captures one real authored/cooked project Scene and its selected schema/resource authorities.
     * @param options Explicit admitted roots/Scene selector/backend/load budget.
     * @return Owned resources with running CPU workers, or the original typed capture/validation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<HeadlessProjectSceneResources>> PrepareHeadlessProjectSceneResources(
        const HeadlessProjectSceneOptions &options);
}  // namespace Horo::Application::Internal
