#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationTileArtifact.h"

namespace Horo::Navigation::Detail {
    /** @brief Detached common-grid topology and its bounded translation storage. */
    struct RecastDetourJoinedTileTopology final {
        NavigationTileBuildResult topology;
        std::size_t reservedBytes{};
    };

    /** @brief Joins exact reciprocal tile boundary edges while retaining local cooked neighbors and authored links. */
    [[nodiscard]] Result<RecastDetourJoinedTileTopology> JoinRecastDetourCookedTiles(
        std::span<const std::shared_ptr<const NavigationCookedTile>> tiles, std::size_t maximumOwnedBytes);
}  // namespace Horo::Navigation::Detail
