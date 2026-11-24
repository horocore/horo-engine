#pragma once

/** @file EditorWorkspaceHostServices.h @brief Private inert value groups for the workspace composition root. */

#include "Horo/Application/WorldBakeApplication.h"

#include <memory>
#include <optional>

namespace Horo {
    class OperationStore;
    class BuildOutputStore;
}  // namespace Horo

namespace Horo::Assets {
    class AssetRegistry;
    class IAssetRegistrySource;
    class AssetImporterCatalogSnapshot;
}  // namespace Horo::Assets

namespace Horo::Platform {
    class IProjectReadFiles;
}

namespace Horo::Editor {
    class PartitionedWorldWorkspace;
    struct WorldStreamingPreviewRuntimePolicy;
    class WorldStreamingPreviewHost;

    /** @brief Host asset catalog inputs; the registry and importer publisher outlive the workspace. */
    struct EditorWorkspaceAssetServices {
        Assets::AssetRegistry *mutableAssetRegistry{};
        std::shared_ptr<const Assets::IAssetRegistrySource> source;
        const Assets::AssetImporterCatalogSnapshot *importerCatalog{};
    };

    /** @brief One document incarnation and its explicit canonical world/source and preview owners. */
    struct EditorWorkspaceWorldServices {
        PartitionedWorldWorkspace *partitionedWorld{};                  /**< Borrowed host-owned canonical world; outlives this session. */
        std::shared_ptr<const Platform::IProjectReadFiles> sourceFiles; /**< Retained project root; no draw-path I/O. */
        Assets::AssetId documentSessionOwner; /**< Host-issued never-reused workspace incarnation; no ambient generation. */
        const WorldStreamingPreviewRuntimePolicy *streamingPreviewPolicy{};
        WorldStreamingPreviewHost *streamingPreview{}; /**< Borrowed application-owned capture bridge; outlives this workspace. */
    };

    /** @brief World-bake stores and immutable project/dependency captures; the shared job system stays outside this group. */
    struct EditorWorkspaceWorldBakeServices {
        OperationStore *operations{};
        BuildOutputStore *output{};
        std::optional<Application::WorldBakeProjectCapture> project;
        std::optional<Application::WorldBakeDependencySource> dependencies;
    };

}  // namespace Horo::Editor
