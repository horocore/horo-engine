#pragma once

/**
 * @file NavigationMeshBuilder.h
 * @brief Provider-neutral bounded contract for one grounded NavMesh tile build.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavMeshData.h"
#include "Horo/Navigation/NavigationBakeInput.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Navigation {
    /** @brief Distinguishes a valid tile with no walkable surface from a generated tile. */
    enum class NavigationTileBuildState : std::uint8_t {
        Empty,
        Built,
        Count,
    };

    /** @brief Provider-neutral warning categories emitted by a bounded tile build. */
    enum class NavigationTileBuildWarningCode : std::uint8_t {
        NonWalkableTrianglesDiscarded,
        RegionsDiscarded,
        ProviderWarning,
        EmptyTile,
        Count,
    };

    /** @brief One aggregated warning category and its deterministic occurrence count. */
    struct NavigationTileBuildWarning final {
        NavigationTileBuildWarningCode code{NavigationTileBuildWarningCode::ProviderWarning}; /**< Stable warning category. */
        std::uint32_t occurrences{}; /**< Saturating number of observations represented by this row. */

        [[nodiscard]] constexpr auto operator<=>(const NavigationTileBuildWarning &) const noexcept = default;
    };

    /** @brief Provider-neutral deterministic facts collected while generating one tile. */
    struct NavigationTileBuildStatistics final {
        std::uint64_t inputTriangleCount{};      /**< Borrowed input triangle count before tile intersection. */
        std::uint64_t walkableTriangleCount{};   /**< Triangles accepted by the slope walkability filter. */
        std::uint64_t rasterizedSpanCount{};     /**< Heightfield spans produced before compacting. */
        std::uint64_t regionCount{};             /**< Retained compact-heightfield regions. */
        std::uint64_t contourCount{};            /**< Simplified contours provided to polygonization. */
        std::uint64_t vertexCount{};             /**< Owned neutral polygon vertices. */
        std::uint64_t polygonCount{};            /**< Owned neutral polygons. */
        std::uint64_t polygonVertexIndexCount{}; /**< Owned polygon-to-vertex indices. */
        std::uint64_t polygonAdjacencyCount{};   /**< Owned polygon edge adjacency entries. */
        std::uint64_t outputOwnedBytes{};        /**< Measured vector-capacity bytes in the returned output. */

        [[nodiscard]] constexpr auto operator<=>(const NavigationTileBuildStatistics &) const noexcept = default;
    };

    /** @brief Hard ceilings for one provider-neutral tile-build invocation. */
    struct NavigationTileBuildLimits final {
        static constexpr std::uint32_t MaximumVertices = 65'534;
        static constexpr std::uint32_t MaximumPolygons = 32'768;
        static constexpr std::uint32_t MaximumOffMeshLinks = 4'096;
        static constexpr std::uint32_t MaximumVerticesPerPolygon = 6;
        static constexpr std::uint64_t MaximumOwnedBytes = 256ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumWorkUnits = 64ULL * 1024ULL * 1024ULL;

        std::uint32_t maximumVertices{MaximumVertices};                     /**< Maximum neutral vertices returned by one tile. */
        std::uint32_t maximumPolygons{MaximumPolygons};                     /**< Maximum neutral polygons returned by one tile. */
        std::uint32_t maximumOffMeshLinks{MaximumOffMeshLinks};             /**< Maximum links retained in one tile result. */
        std::uint32_t maximumVerticesPerPolygon{MaximumVerticesPerPolygon}; /**< Maximum corners in one polygon. */
        std::uint64_t maximumOwnedBytes{MaximumOwnedBytes};                 /**< Maximum measured output and bounded work storage. */
        std::uint64_t maximumWorkUnits{MaximumWorkUnits};                   /**< Maximum deterministic voxelization work estimate. */

        [[nodiscard]] constexpr auto operator<=>(const NavigationTileBuildLimits &) const noexcept = default;
    };

    /** @brief Immutable borrowed inputs and exact tile bounds for one grounded tile build. */
    struct NavigationTileBuildRequest final {
        NavMeshTileKey key;                                     /**< Exact horizontal tile coordinate and layer. */
        Math::Aabb bounds;                                      /**< Canonical bounds; X/Z must match key and tileSizeMeters. */
        float tileSizeMeters{};                                 /**< Canonical horizontal tile extent. */
        NavigationAgentBuildGeometry buildGeometry{};           /**< Validated grounded profile voxel settings. */
        std::span<const NavigationTileBuildTriangle> triangles; /**< Borrowed canonical source triangles. */
        std::span<const NavigationTileBuildModifier> modifiers; /**< Borrowed area/exclusion volumes. */
        NavigationTileBuildLimits limits{};                     /**< Caller-selected ceilings no greater than hard limits. */
    };

    /** @brief Owned provider-neutral tile output with portable topology and source provenance. */
    struct NavigationTileBuildResult final {
        NavigationTileBuildState state{NavigationTileBuildState::Empty}; /**< Built or valid-empty outcome state. */
        NavMeshTileKey key;                                              /**< Exact request tile key copied into the result. */
        Math::Aabb bounds;                                               /**< Exact request bounds copied into the result. */
        NavigationTileBuildStatistics statistics;                        /**< Provider-neutral build facts. */
        std::vector<Math::Vec3> vertices;                                /**< Owned canonical polygon vertices. */
        std::vector<NavMeshPolygon> polygons;                            /**< Owned polygon table. */
        std::vector<std::uint32_t> polygonVertexIndices;                 /**< Owned polygon vertex-index table. */
        std::vector<std::uint32_t> polygonAdjacencies;                   /**< Owned polygon edge-neighbor table. */
        std::vector<NavMeshOffMeshLink> offMeshLinks;                    /**< Owned generated links, if the provider supplies any. */
        std::vector<NavMeshSourceProvenance> provenance;                 /**< Owned source evidence ranges. */
        std::vector<NavigationTileBuildWarning> warnings;                /**< Owned aggregated warnings. */

        /** @brief Reports whether this valid result contains no generated walkable polygons. */
        [[nodiscard]] constexpr bool IsEmpty() const noexcept {
            return state == NavigationTileBuildState::Empty;
        }
    };

    /**
     * @brief Provider-neutral synchronous seam for one already-admitted grounded tile build.
     *
     * The builder owns no scheduler, cache, publication path, operation record, or source lifetime.
     * Callers retain the borrowed request ranges for the duration of the call and publish only a
     * complete result returned from this transaction.
     */
    class INavigationMeshBuilder {
    public:
        virtual ~INavigationMeshBuilder() = default;

        /**
         * @brief Generates one deterministic tile from canonical triangles and modifiers.
         * @param request Bounded tile geometry, profile settings, and borrowed canonical inputs.
         * @param cancellation Cooperative cancellation observed at bounded pipeline boundaries.
         * @return Built or valid-empty tile, or a typed validation, capacity, cancellation, or provider failure.
         */
        [[nodiscard]] virtual Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                                          const CancellationToken &cancellation) const = 0;
    };
}  // namespace Horo::Navigation
