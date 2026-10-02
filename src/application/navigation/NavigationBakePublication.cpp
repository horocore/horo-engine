#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>

namespace Horo::Application {
    /** @copydoc ResolveNavigationBakePublication */
    Result<std::shared_ptr<const NavigationBakePublication>> ResolveNavigationBakePublication(const NavigationBakeServiceConfig &config) {
        using namespace Navigation;
        if (config.maximumCandidateBytes == 0 || config.maximumCandidateBytes > NavMeshArtifactLimits::MaximumOwnedBytes ||
            config.maximumTiles == 0 || config.maximumTiles > NavMeshArtifactLimits::MaximumTiles)
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(
                MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
        auto generation = Assets::ResolveCurrentCookGeneration(config.targetRoot, config.cookLimits);
        if (generation.HasError())
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(generation.ErrorValue());
        if (generation.Value().target != config.target)
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
        auto contents = Assets::ReadCookGenerationContents(generation.Value(), config.maximumCandidateBytes, config.cookLimits);
        if (contents.HasError())
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(contents.ErrorValue());
        const auto found =
            std::ranges::lower_bound(contents.Value().entries, config.definition, {}, &Assets::AssetCookManifestEntry::assetId);
        if (found == contents.Value().entries.end() || found->assetId != config.definition || found->assetType != config.artifactType)
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
        const auto index = static_cast<std::size_t>(found - contents.Value().entries.begin());
        auto artifact = Assets::DecodeCookedArtifact(contents.Value().artifacts[index], config.cookLimits);
        if (artifact.HasError())
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(artifact.ErrorValue());
        auto tiles = DecodeNavigationCookedTileSet(artifact.Value().payload, config.maximumCandidateBytes);
        if (tiles.HasError())
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(tiles.ErrorValue());
        if (tiles.Value().inputFingerprint != artifact.Value().sourceDigest || tiles.Value().tiles.size() > config.maximumTiles)
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
        for (const auto &tile : tiles.Value().tiles) {
            const auto &topology = tile->Topology();
            if (tile->StorageBytes() > config.tileLimits.maximumOwnedBytes ||
                topology.vertices.size() > config.tileLimits.maximumVertices ||
                topology.polygons.size() > config.tileLimits.maximumPolygons ||
                topology.offMeshLinks.size() > config.tileLimits.maximumOffMeshLinks ||
                std::ranges::any_of(topology.polygons, [&config](const auto &polygon) {
                return polygon.vertexIndices.count > config.tileLimits.maximumVerticesPerPolygon;
            }))
                return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(
                    MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
        }
        auto publication = std::make_shared<NavigationBakePublication>();
        publication->generation = std::move(generation).Value();
        publication->tiles = std::move(tiles).Value();
        return Result<std::shared_ptr<const NavigationBakePublication>>::Success(std::move(publication));
    }
}  // namespace Horo::Application
