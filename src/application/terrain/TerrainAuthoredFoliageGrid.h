#pragma once
#include "Horo/Terrain/FoliagePlacementCook.h"

namespace Horo::Application::TerrainCookDetail {
    /** @brief Source-grid rectangle of one verified first-LOD tile, including its closed terminal edge. */
    struct FoliageTileRegion final {
        Terrain::TerrainTileId tile;
        std::uint32_t x{}, z{}, width{}, height{};
    };
    /** @brief Own sample storage before exposing its temporary immutable view; move preserves the vector allocation. */
    struct FoliageTileGrid final {
        std::vector<Terrain::FoliagePlacementSample> samples;
        Terrain::FoliagePlacementGrid view;
        FoliageTileGrid() = default;
        FoliageTileGrid(FoliageTileGrid &&) noexcept = default;
        FoliageTileGrid &operator=(FoliageTileGrid &&) noexcept = default;
        FoliageTileGrid(const FoliageTileGrid &) = delete;
        FoliageTileGrid &operator=(const FoliageTileGrid &) = delete;
    };
    [[nodiscard]] Result<FoliageTileGrid> BuildFoliageGrid(const Terrain::TerrainCanonicalSource &source,
        const FoliageTileRegion &region, const CancellationToken &cancellation);
}
