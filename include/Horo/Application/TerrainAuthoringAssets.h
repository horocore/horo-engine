#pragma once
/** @file TerrainAuthoringAssets.h @brief Explicit creation of tracked terrain layer sets and foliage definition sources. */

#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"

namespace Horo::Application {
    enum class TerrainAuthoringAssetKind : std::uint8_t {
        LayerSet,
        FoliageType
    };

    /** @brief Cold modal intent; every selected material/mesh is an actual project AssetId, never a display-index identity. */
    struct TerrainAuthoringAssetRequest final {
        std::filesystem::path root;
        ProjectPath destination;
        TerrainAuthoringAssetKind kind;
        std::vector<Assets::AssetId> materials;
        Terrain::FoliageTypeDefinitionData foliage;
    };

    /** @brief Source creation receipt survives durability uncertainty; the original typed error remains visible. */
    struct TerrainAuthoringAssetCreated final {
        Assets::AssetId asset;
        ProjectPath destination;
        ProjectAssetImportReceipt publication;
        std::optional<Error> failure;
    };

    /** @brief Actual registry, source identity allocator and shared native publication authority, all explicitly composed. */
    struct TerrainAuthoringAssetEnvironment final {
        Assets::AssetRegistry &registry;
        Editor::ProjectMutationCoordinator &mutations;
        DurableFileSystem &files;
        std::function<Result<Assets::AssetId>()> newIdentity;
        Terrain::TerrainConfigurationSnapshot configuration;
    };

    /** @brief Validate real selected references, reserve semantic identities and publish ordinary tracked source/sidecar bytes.
     * @param environment Explicit owner-lane host authorities. @param request Complete typed creation intent.
     * @param cancellation Checked by the ordinary shared source publication transition.
     * @return Prepared/publication receipt, including post-native failure, or prepublication typed admission failure.
     * @details This produces no renderer material source or compiled mesh/material. Project-provided core.material and
     * core.mesh records are required. Source bytes become an actual dependency capture only during the later cook.
     */
    [[nodiscard]] Result<TerrainAuthoringAssetCreated> CreateTerrainAuthoringAsset(const TerrainAuthoringAssetEnvironment &environment,
                                                                                   const TerrainAuthoringAssetRequest &request,
                                                                                   const CancellationToken &cancellation = {});
    /** @brief Explicit UI composition, invoked only by modal actions. */
    using TerrainAuthoringAssetFactory = std::function<Result<TerrainAuthoringAssetCreated>(const TerrainAuthoringAssetRequest &)>;
}  // namespace Horo::Application
