#pragma once

/** @file NavigationTileDependencies.h
 * @brief Exact, halo-aware dependency capture for incremental grounded tile baking.
 */

#include "Horo/Navigation/NavigationMeshBuilder.h"

namespace Horo::Navigation {
    /** @brief Stable partition and grid address; generated partitions are not authoring assets. */
    struct NavigationBakeTileKey final {
        NavigationAgentProfileId profile;
        SurfaceId surface;
        NavMeshTileKey tile;
        [[nodiscard]] constexpr auto operator<=>(const NavigationBakeTileKey &) const noexcept = default;
    };

    /** @brief Complete host-resolved compatibility closure, independent of source edit revisions. */
    struct NavigationTileBakeCompatibility final {
        Sha256Digest provider; /**< Exact provider source/options/compiler/architecture/FP fingerprint. */
        Sha256Digest schemas;  /**< Project, source, coordinate, tile, neutral/envelope format and catalog schema closure. */
        Sha256Digest settings; /**< Definition policy, target/profile, package lock and cooker identity/version closure. */
        [[nodiscard]] auto operator<=>(const NavigationTileBakeCompatibility &) const noexcept = default;
    };

    /** @brief One requested tile in the complete replacement closure. */
    struct NavigationBakeTile final {
        NavigationBakeTileKey key;
        Math::Aabb bounds;
        float tileSizeMeters{};
    };

    /** @brief Owned canonical dependency subset supplied identically to hashing and voxelization. */
    struct NavigationPreparedTile final {
        NavigationBakeTile tile;
        NavigationAgentBuildGeometry geometry;
        std::uint32_t borderSizeCells{};
        Sha256Digest dependencyKey;
        std::vector<NavigationTileBuildTriangle> triangles;
        std::vector<NavigationTileBuildModifier> modifiers;
    };

    /**
     * @brief Captures the exact conservative spatial read set for one tile, including the provider border halo.
     * @param input Canonical immutable source snapshot.
     * @param tile Exact partition/grid address and bounds; the tile size must be an integral voxel count.
     * @param compatibility Complete nonzero semantic compatibility digests resolved by the host.
     * @param cancellation Cooperative operation cancellation.
     * @param maximumOwnedBytes Positive selected-input storage ceiling, including vector growth.
     * @return Owned input subset and versioned dependency key, or typed invalid/capacity/cancellation failure.
     * @note Source and modifier removal/movement changes both old and new footprints by recomputing the complete closure.
     * Scene/request/source-snapshot revisions fence adoption; they do not invalidate unrelated tile subsets.
     */
    [[nodiscard]] Result<NavigationPreparedTile> PrepareNavigationBakeTile(
        const NavigationBakeInputSnapshot &input, const NavigationBakeTile &tile, const NavigationTileBakeCompatibility &compatibility,
        const CancellationToken &cancellation = {}, std::size_t maximumOwnedBytes = NavigationBakeInputLimits::MaximumOwnedBytes);
}  // namespace Horo::Navigation
