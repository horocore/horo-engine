#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        /** @brief Pins the requested definition inside the verified complete manifest before decoding its owned payload. */
        [[nodiscard]] Result<Navigation::NavigationCookedTileSet> ReadDefinition(const Assets::AssetCookGeneration &generation,
                                                                                 const NavigationBakeServiceConfig &config) {
            auto contents = Assets::ReadCookGenerationContents(generation, config.maximumCandidateBytes, config.cookLimits);
            if (contents.HasError())
                return Result<Navigation::NavigationCookedTileSet>::Failure(contents.ErrorValue());
            const auto found =
                std::ranges::lower_bound(contents.Value().entries, config.definition, {}, &Assets::AssetCookManifestEntry::assetId);
            if (found == contents.Value().entries.end() || found->assetId != config.definition || found->assetType != config.artifactType)
                return Result<Navigation::NavigationCookedTileSet>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            const auto index = static_cast<std::size_t>(found - contents.Value().entries.begin());
            auto artifact = Assets::DecodeCookedArtifact(contents.Value().artifacts[index], config.cookLimits);
            if (artifact.HasError())
                return Result<Navigation::NavigationCookedTileSet>::Failure(artifact.ErrorValue());
            auto tiles = Navigation::DecodeNavigationCookedTileSet(artifact.Value().payload, config.maximumCandidateBytes);
            if (tiles.HasError())
                return tiles;
            if (tiles.Value().inputFingerprint != artifact.Value().sourceDigest || tiles.Value().tiles.size() > config.maximumTiles)
                return Result<Navigation::NavigationCookedTileSet>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            return tiles;
        }

        /** @brief Applies caller topology limits in addition to the decoder's portable ceilings. */
        [[nodiscard]] bool WithinTileLimits(const Navigation::NavigationCookedTile &tile,
                                            const Navigation::NavigationTileBuildLimits &limits) {
            const auto &topology = tile.Topology();
            return tile.StorageBytes() <= limits.maximumOwnedBytes && topology.vertices.size() <= limits.maximumVertices &&
                   topology.polygons.size() <= limits.maximumPolygons && topology.offMeshLinks.size() <= limits.maximumOffMeshLinks &&
                   std::ranges::all_of(topology.polygons, [&limits](const auto &polygon) {
                return polygon.vertexIndices.count <= limits.maximumVerticesPerPolygon;
            });
        }
    }  // namespace

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
        auto tiles = ReadDefinition(generation.Value(), config);
        if (tiles.HasError())
            return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(tiles.ErrorValue());
        for (const auto &tile : tiles.Value().tiles) {
            if (!WithinTileLimits(*tile, config.tileLimits))
                return Result<std::shared_ptr<const NavigationBakePublication>>::Failure(
                    MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
        }
        auto publication = std::make_shared<NavigationBakePublication>();
        publication->generation = std::move(generation).Value();
        publication->tiles = std::move(tiles).Value();
        return Result<std::shared_ptr<const NavigationBakePublication>>::Success(std::move(publication));
    }
}  // namespace Horo::Application
