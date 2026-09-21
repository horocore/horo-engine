#pragma once

/**
 * @file RecastDetourProvider.h
 * @brief Horo-owned composition contract for the default grounded Detour query provider.
 */

#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationBackend.h"
#include "Horo/Navigation/NavigationMeshBuilder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Navigation {
    /** @brief One validated convex grounded polygon in provider-neutral vertex-index form. */
    struct GroundedNavigationPolygon final {
        std::array<std::uint32_t, 6> vertexIndices{}; /**< Counter-clockwise canonical-space indices; unused entries are zero. */
        std::uint8_t vertexCount{};                   /**< Active prefix length in [3, 6]. */
        NavigationAreaId area;                        /**< Stable Horo area identity mapped privately during preparation. */
    };

    /** @brief Hard qualification ceilings for the initial Detour runtime adapter. */
    struct RecastDetourProviderHardLimits final {
        static constexpr std::uint32_t Vertices = 65'534;
        static constexpr std::uint32_t Polygons = 32'768;
        static constexpr std::uint32_t QueryNodes = 65'536;
        static constexpr std::uint32_t ResultPoints = 4'096;
        static constexpr std::uint32_t ConcurrentQueries = 64;
        static constexpr std::size_t OwnedBytes = 512ULL * 1024ULL * 1024ULL;
    };

    /** @brief Borrowed immutable topology and bounded provider preparation policy. */
    struct RecastDetourProviderCreateInfo final {
        NavigationWorldId world;                                  /**< Exact navigation-world incarnation. */
        NavigationGeneration topology;                            /**< Exact immutable topology generation. */
        std::span<const Math::Vec3> vertices;                     /**< Finite canonical-metre vertices. */
        std::span<const GroundedNavigationPolygon> polygons;      /**< Convex polygons addressing vertices. */
        Math::Vec3 nearestPointHalfExtents{2.0F, 4.0F, 2.0F};     /**< Positive finite Detour projection half-extents. */
        float cellSizeMeters{0.3F};                               /**< Positive finite horizontal quantization unit. */
        float cellHeightMeters{0.2F};                             /**< Positive finite vertical quantization unit. */
        float walkableHeightMeters{2.0F};                         /**< Positive finite agent height. */
        float walkableRadiusMeters{0.5F};                         /**< Finite non-negative agent radius. */
        float walkableClimbMeters{0.4F};                          /**< Finite non-negative maximum step. */
        std::uint32_t maximumQueryNodes{2'048};                   /**< Fixed node-pool size prepared per query lease. */
        std::uint32_t maximumResultPoints{256};                   /**< Fixed path and straight-path scratch capacity. */
        std::uint32_t maximumConcurrentQueries{1};                /**< Non-blocking query-lease pool size. */
        float maximumSearchDistanceMeters{10'000.0F};             /**< Capability-advertised finite request ceiling. */
        std::size_t maximumOwnedBytes{64ULL * 1024ULL * 1024ULL}; /**< Preparation-time owned-memory admission ceiling. */
        std::uint64_t capabilityRevision{1};                      /**< Non-zero immutable capability evidence revision. */
    };

    /**
     * @brief Transactionally creates the default grounded query provider from neutral polygon topology.
     * @param info Borrowed topology and fixed resource limits copied during the call.
     * @return Independently owned query backend, or a typed invalid, corrupt, capacity, or allocation failure.
     * @post Failure publishes no provider and releases every partially created native object.
     * @note The returned provider accepts concurrent calls only up to maximumConcurrentQueries; excess work fails admission without
     * blocking.
     */
    [[nodiscard]] Result<std::unique_ptr<INavigationQueryBackend>> CreateRecastDetourNavigationQueryBackend(
        const RecastDetourProviderCreateInfo &info);

    /**
     * @brief Creates the pinned Recast grounded tile builder.
     * @return Independently owned tile builder, or a typed unsupported result when the provider is omitted.
     */
    [[nodiscard]] Result<std::unique_ptr<INavigationMeshBuilder>> CreateRecastDetourNavigationMeshBuilder();
}  // namespace Horo::Navigation
