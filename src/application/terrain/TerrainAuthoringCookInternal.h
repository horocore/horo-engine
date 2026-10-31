#pragma once
/** @file TerrainAuthoringCookInternal.h @brief Private joined source-capture and publication phases. */
#include "Horo/Application/TerrainAuthoringCook.h"

namespace Horo::Application::TerrainCookDetail {
    /** @brief One immutable actual-file closure; no document or runtime/native owner crosses into CPU jobs. */
    struct Capture final {
        std::shared_ptr<const Assets::AssetCookInputSnapshot> inputs;
        Terrain::TerrainAuthoringSource source;
        std::vector<Terrain::FoliageTypeSource> definitions;
        std::vector<Assets::AssetId> dependencies;
    };

    [[nodiscard]] Result<Capture> CaptureSources(const TerrainAuthoringCookEnvironment &environment,
                                                 const TerrainAuthoringCookRequest &request, const CancellationToken &cancellation);
    [[nodiscard]] Result<void> VerifySources(const TerrainAuthoringCookEnvironment &environment, const TerrainAuthoringCookRequest &request,
                                             const Capture &capture, const CancellationToken &cancellation);
    [[nodiscard]] Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> MakeCatalog(std::shared_ptr<const Capture> capture,
                                                                                           const TerrainAuthoringCookRequest &request,
                                                                                           const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<std::uint8_t>> MakeBundle(const Capture &capture, const TerrainAuthoringCookProfile &profile,
                                                               const CancellationToken &cancellation);
    [[nodiscard]] Sha256Digest ProfileDigest(const TerrainAuthoringCookProfile &profile);
    /** @brief Adapt actual canonical source samples and verified LOD0 membership into sealed authored placement evidence. */
    [[nodiscard]] Result<std::vector<Terrain::CookedFoliagePlacement>> CookAuthoredFoliage(const Capture &capture,
        const Terrain::CookedTerrainTileSet &tiles, const TerrainAuthoringCookProfile &profile,
        const CancellationToken &cancellation);
    /** @brief Run one complete admitted attempt on the feature control thread; all CPU children join before return. */
    [[nodiscard]] Result<Assets::AssetCookReport> CookAttempt(const TerrainAuthoringCookEnvironment &environment,
                                                              const TerrainAuthoringCookRequest &request,
                                                              const CancellationToken &cancellation);
}  // namespace Horo::Application::TerrainCookDetail
