#pragma once

/**
 * @file NavigationBackend.h
 * @brief Provider-neutral grounded navigation query execution contract.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationCapabilities.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace Horo::Navigation {
    /** @brief Whether a path query may publish a bounded best-effort prefix. */
    enum class NavigationPathCoveragePolicy : std::uint8_t {
        RequireComplete,
        AllowPartial,
        Count
    };

    /** @brief Typed state of a provider-neutral grounded path query. */
    enum class NavigationPathStatus : std::uint8_t {
        Reachable,
        Complete = Reachable,
        Partial,
        Unreachable,
        BudgetExceeded,
        Count
    };

    /** @brief Explains why a path stopped before reaching the requested destination. */
    enum class NavigationPathStopReason : std::uint8_t {
        None,
        DestinationUnreachable,
        NodeBudgetExceeded,
        SearchBudgetExceeded = NodeBudgetExceeded,
        ResultPointBudgetExceeded,
        OutputBudgetExceeded = ResultPointBudgetExceeded,
        Count
    };

    /** @brief Sentinel used when a path result has no known canonical polygon frontier. */
    inline constexpr std::uint32_t NavigationPathNoPolygon = std::numeric_limits<std::uint32_t>::max();

    /** @brief Bounded provider-neutral request for one grounded path. */
    struct NavigationPathRequest final {
        NavigationWorldId world;       /**< Exact active world captured at admission. */
        NavigationGeneration topology; /**< Exact immutable topology captured at admission. */
        Math::Vec3 start;              /**< Start in Horo right-handed, Y-up world space. */
        Math::Vec3 destination;        /**< Destination in the same origin revision as start. */
        NavigationFilterId filter;     /**< Exact registered traversal filter; never a default fallback. */
        NavigationPathCoveragePolicy coveragePolicy{NavigationPathCoveragePolicy::RequireComplete}; /**< Partial-path opt-in. */
        NavigationQueryRequirement requirement; /**< Exact admitted quality and execution bounds. */
    };

    /**
     * @brief Provider-neutral ordered path points with typed progress and cost evidence.
     * @details Reachable paths end at the requested destination. Partial paths end at `stopPosition` and carry the
     * reason and canonical polygon where progress stopped. Unreachable and budget-exhausted paths may have no points.
     */
    struct NavigationPath final {
        std::vector<Math::Vec3> points;                                 /**< Ordered world-space points, including the progress endpoint. */
        NavigationPathStatus status{NavigationPathStatus::Unreachable}; /**< Typed search result state. */
        NavigationPathStopReason stopReason{NavigationPathStopReason::None}; /**< Why progress stopped, if it did. */
        Math::Vec3 stopPosition{};                               /**< Finite progress location for partial/unreachable results. */
        std::uint32_t stopPolygonIndex{NavigationPathNoPolygon}; /**< Canonical polygon at the progress frontier, when known. */
        float cost{};                                            /**< Finite non-negative traversal cost of the returned route. */
        float lengthMeters{};                                    /**< Finite non-negative geometric path length in metres. */
        NavigationGeneration sourceGeneration;                   /**< Exact topology generation used to produce this path. */
    };

    /**
     * @brief Provider-neutral provenance for one polygon observed by a bounded spatial query.
     *
     * The polygon index is the canonical index supplied in the provider creation topology. It is not a native
     * polygon reference and is valid only with the exact world and topology generation carried beside it. A surface
     * identity may be invalid when the composed topology has no authored surface binding.
     */
    struct NavigationQueryProvenance final {
        NavigationWorldId world;       /**< Exact navigation-world incarnation that was queried. */
        NavigationGeneration topology; /**< Exact immutable topology generation that was queried. */
        SurfaceId surface;             /**< Stable authored surface identity, when supplied by the topology owner. */
        std::uint32_t polygonIndex{};  /**< Canonical provider-neutral polygon index. */

        [[nodiscard]] constexpr auto operator<=>(const NavigationQueryProvenance &) const noexcept = default;
    };

    /**
     * @brief One provider-neutral grounded-surface observation returned by projection, sampling, polygon, or raycast queries.
     *
     * Multi-result observations are ordered by ascending distance and then ascending canonical polygon index. The
     * normal is finite and points away from the grounded surface; provider-native references never cross this boundary.
     */
    struct NavigationSurfaceHit final {
        Math::Vec3 position{};                /**< Finite point on the observed navigation surface. */
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};  /**< Finite normalized surface or blocking-edge normal. */
        float distanceMeters{};               /**< Finite non-negative distance from the query's source point. */
        SurfaceId surface;                    /**< Stable authored surface identity, when supplied by the topology owner. */
        NavigationAreaId area;                /**< Stable traversal-area identity of the observed polygon. */
        NavigationQueryProvenance provenance; /**< Exact world, topology, surface, and polygon evidence. */

        [[nodiscard]] constexpr std::partial_ordering operator<=>(const NavigationSurfaceHit &other) const noexcept {
            if (const auto order = distanceMeters <=> other.distanceMeters; order != 0)
                return order;
            if (const auto order = provenance.polygonIndex <=> other.provenance.polygonIndex; order != 0)
                return order;
            return std::tie(position.x, position.y, position.z, normal.x, normal.y, normal.z, surface, area, provenance) <=>
                   std::tie(other.position.x, other.position.y, other.position.z, other.normal.x, other.normal.y, other.normal.z,
                            other.surface, other.area, other.provenance);
        }

        [[nodiscard]] constexpr bool operator==(const NavigationSurfaceHit &) const noexcept = default;
    };

    /** @brief Bounded point-projection request against one exact navigation world and topology generation. */
    struct NavigationPointProjectionRequest final {
        NavigationWorldId world;                /**< Exact active world captured by the caller. */
        NavigationGeneration topology;          /**< Exact immutable topology captured by the caller. */
        Math::Vec3 point;                       /**< Finite point to project onto a reachable navigation surface. */
        Math::Vec3 halfExtents{};               /**< Optional finite positive search half-extents; zero selects provider defaults. */
        NavigationQueryRequirement requirement; /**< Exact nearest-point quality and work/output bounds. */
    };

    /** @brief Bounded request for reachable surface samples within one local radius. */
    struct NavigationSamplePositionRequest final {
        NavigationWorldId world;                /**< Exact active world captured by the caller. */
        NavigationGeneration topology;          /**< Exact immutable topology captured by the caller. */
        Math::Vec3 center;                      /**< Finite center of the reachable sampling neighbourhood. */
        float radiusMeters{};                   /**< Positive finite reachable radius in metres. */
        NavigationQueryRequirement requirement; /**< Exact sampling quality and result bound. */
    };

    /** @brief Bounded navigation-surface raycast request between two finite points. */
    struct NavigationRaycastRequest final {
        NavigationWorldId world;                /**< Exact active world captured by the caller. */
        NavigationGeneration topology;          /**< Exact immutable topology captured by the caller. */
        Math::Vec3 start;                       /**< Finite ray origin. */
        Math::Vec3 destination;                 /**< Finite ray destination. */
        NavigationQueryRequirement requirement; /**< Exact raycast quality and traversal/result bound. */
    };

    /** @brief Bounded axis-aligned polygon-overlap query request. */
    struct NavigationPolygonQueryRequest final {
        NavigationWorldId world;                /**< Exact active world captured by the caller. */
        NavigationGeneration topology;          /**< Exact immutable topology captured by the caller. */
        Math::Vec3 center;                      /**< Finite center of the search box. */
        Math::Vec3 halfExtents;                 /**< Positive finite search half-extents. */
        NavigationQueryRequirement requirement; /**< Exact polygon-query quality and result bound. */
    };

    /** @brief One projection result with stable grounded-surface metadata. */
    struct NavigationProjectionResult final {
        NavigationSurfaceHit hit;
    };

    /**
     * @brief Bounded reachable-sample result.
     * @details Samples are sorted by `(distanceMeters, provenance.polygonIndex)`. `truncated` reports that additional
     * valid candidates existed after the caller's result limit; the returned vector never exceeds that limit.
     */
    struct NavigationSamplePositionResult final {
        std::vector<NavigationSurfaceHit> samples;
        bool truncated{};
    };

    /**
     * @brief Bounded navigation raycast result.
     * @details `traversedPolygons` is in origin-to-terminal traversal order. `blocked` is true only when a navigation
     * boundary stopped the ray; an unblocked result reaches the requested destination under the exact topology.
     */
    struct NavigationRaycastResult final {
        NavigationSurfaceHit hit;
        bool blocked{};
        std::vector<NavigationQueryProvenance> traversedPolygons;
        bool truncated{};
    };

    /**
     * @brief Bounded ordered polygon-query result.
     * @details Results are sorted by `(distanceMeters, provenance.polygonIndex)`. `truncated` reports omitted valid
     * candidates while guaranteeing that the owned result buffer never exceeds the caller/profile bound.
     */
    struct NavigationPolygonQueryResult final {
        std::vector<NavigationSurfaceHit> polygons;
        bool truncated{};
    };

    /**
     * @brief Synchronous provider execution seam invoked only from NavigationRuntime-owned work.
     *
     * This interface owns no scheduling, callbacks, world lifetime, or completion publication.
     * The runtime validates capabilities and bounds before dispatch, supplies an immutable
     * topology generation, and translates returned typed errors into terminal outcomes.
     */
    class INavigationQueryBackend {
    public:
        virtual ~INavigationQueryBackend() = default;

        /** @brief Returns immutable composition-time capability evidence by value. */
        [[nodiscard]] virtual NavigationProviderCapabilities Capabilities() const noexcept = 0;

        /**
         * @brief Executes one already-admitted grounded path query.
         * @param request Owned provider-neutral request for an exact world and topology.
         * @param cancellation Cooperative cancellation observed during provider work.
         * @return Path, or a typed Horo navigation error without provider-native values.
         */
        [[nodiscard]] virtual Result<NavigationPath> FindPath(const NavigationPathRequest &request,
                                                              const CancellationToken &cancellation) const = 0;

        /**
         * @brief Projects one finite point onto the nearest reachable polygon.
         * @param request Exact world/topology, point, optional search extents, and bounded query requirement.
         * @param cancellation Cooperative cancellation observed before and during provider work.
         * @return Ordered projection metadata or a typed navigation failure.
         * @note Legacy providers that do not expose spatial primitives return OperationUnsupported until overridden.
         */
        [[nodiscard]] virtual Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                                              const CancellationToken &cancellation) const {
            static_cast<void>(request);
            static_cast<void>(cancellation);
            return Result<NavigationProjectionResult>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        }

        /**
         * @brief Samples bounded reachable surface positions around one finite center.
         * @param request Exact world/topology, finite radius, and caller/profile result bounds.
         * @param cancellation Cooperative cancellation observed before and during provider work.
         * @return Stable ordered samples, or a typed navigation failure.
         */
        [[nodiscard]] virtual Result<NavigationSamplePositionResult> SamplePosition(const NavigationSamplePositionRequest &request,
                                                                                    const CancellationToken &cancellation) const {
            static_cast<void>(request);
            static_cast<void>(cancellation);
            return Result<NavigationSamplePositionResult>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        }

        /**
         * @brief Raycasts across reachable navigation polygons from one finite point to another.
         * @param request Exact world/topology, finite endpoints, and caller/profile traversal bounds.
         * @param cancellation Cooperative cancellation observed before and during provider work.
         * @return Blocking-surface metadata and bounded traversal provenance, or a typed navigation failure.
         */
        [[nodiscard]] virtual Result<NavigationRaycastResult> Raycast(const NavigationRaycastRequest &request,
                                                                      const CancellationToken &cancellation) const {
            static_cast<void>(request);
            static_cast<void>(cancellation);
            return Result<NavigationRaycastResult>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        }

        /**
         * @brief Queries reachable polygons overlapping a finite axis-aligned search box.
         * @param request Exact world/topology, finite box, and caller/profile result bounds.
         * @param cancellation Cooperative cancellation observed before and during provider work.
         * @return Stable distance/polygon-index ordered polygon observations, or a typed navigation failure.
         */
        [[nodiscard]] virtual Result<NavigationPolygonQueryResult> QueryPolygons(const NavigationPolygonQueryRequest &request,
                                                                                 const CancellationToken &cancellation) const {
            static_cast<void>(request);
            static_cast<void>(cancellation);
            return Result<NavigationPolygonQueryResult>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        }
    };
}  // namespace Horo::Navigation
