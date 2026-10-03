#pragma once

/** @file NavigationTileDescriptor.h
 * @brief Validated load-time metadata projection owned by the canonical portable tile codec. */
#include "Horo/Navigation/NavigationTileArtifact.h"

namespace Horo::Navigation {
    /** @brief Exact already-validated scalar descriptor used for runtime materialization. */
    struct NavigationCookedTileDescriptor final {
        NavigationAgentBuildGeometry geometry;
        float tileSizeMeters{};
        std::uint32_t borderSizeCells{};
    };

    /** @brief Projects the descriptor from an immutable canonical factory-validated tile.
     * @param tile Complete canonical tile retained by the caller; construction cannot bypass Create/Decode.
     * @return Exact encoded build dimensions/grid/border or typed corruption on an inconsistent tile.
     * @note Fixed-size load-time projection uses the canonical codec's private bounded reader, without
     * a second writer, schema, decode authority or runtime/query I/O. Canonical Create/Decode owns validation. */
    [[nodiscard]] Result<NavigationCookedTileDescriptor> ProjectNavigationCookedTileDescriptor(const NavigationCookedTile &tile);
}  // namespace Horo::Navigation
