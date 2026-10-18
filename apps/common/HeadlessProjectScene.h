#pragma once

/** @file HeadlessProjectScene.h
 * @brief Application-owned project Scene inputs and immutable load resources for one explicit mode host.
 */
#include "CommittedSceneAssets.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Foundation/JobSystem.h"

namespace Horo::Application::Internal {
    /** @brief Explicit Scene input parsed at the CLI boundary, without backend discovery. */
    struct HeadlessProjectSceneOptions final {
        std::filesystem::path projectRoot;
        std::string scene;
        std::filesystem::path cookedRoot;
        std::string cookTarget;
        Physics::PhysicsRuntimeMode physicsMode{Physics::PhysicsRuntimeMode::Null};
        std::chrono::milliseconds loadTimeout{30'000};
    };

    /** @brief Owns all borrowed assets/jobs/project captures until every world service has retired.
     * @details Its operation boundary may be replaced only before an entire owner-lane Startup/Travel call.
     */
    struct HeadlessProjectSceneResources final {
        JobSystem jobs{JobSystemConfig{1, 128}};
        Assets::AssetRegistry registry;
        std::unique_ptr<CommittedSceneAssets> committed;
        Assets::MemoryAssetProvider empty;
        std::unique_ptr<Assets::AssetLoadService> loads;
        std::unique_ptr<Assets::AssetLoadService> prefabLoads;
        std::shared_ptr<const ProjectCollisionSettings> collision;
        CancellationSource cancellation;
        std::chrono::steady_clock::time_point deadline;
        HeadlessSceneLoadBoundary load;
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition;
        std::optional<Physics::PhysicsSceneActivationSettings> settings;
    };

    /** @brief Captures one real authored/cooked project Scene and its selected schema/resource authorities.
     * @param options Explicit admitted roots/Scene selector/backend/load budget.
     * @return Owned resources with running CPU workers, or the original typed capture/validation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<HeadlessProjectSceneResources>> PrepareHeadlessProjectSceneResources(
        const HeadlessProjectSceneOptions &options);
}  // namespace Horo::Application::Internal
