#pragma once

#include "runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h"

namespace Horo::Navigation::RecastDetourQueries {
    struct QueryEndpoint final {
        bool found{};
        std::uint32_t polygon{InvalidNavigationPolygonIndex};
        Math::Vec3 projected{};
    };

    struct CorridorSearch final {
        NavigationPathStatus status{NavigationPathStatus::Unreachable};
        NavigationPathStopReason stopReason{NavigationPathStopReason::DestinationUnreachable};
        std::uint32_t terminalNode{InvalidNavigationPolygonIndex};
        std::uint32_t polygonCount{};
    };

    struct PathBuildContext final {
        QuerySlot &slot;
        const QueryEndpoint &start;
        const NavigationPathRequest &request;
        const CorridorSearch &search;
        const NavigationAreaRegistry &areaRegistry;
        const std::vector<GroundedNavigationPolygon> &polygons;
        const std::vector<Math::Vec3> &vertices;
        const std::vector<Math::Vec3> &centers;
        const std::vector<dtPolyRef> &references;
        std::span<const RecastDetourLinkArc> links;
        float defaultClearanceMeters{};
    };

    [[nodiscard]] Result<QueryEndpoint> ResolveEndpoint(const QuerySlot &slot, Math::Vec3 point, Math::Vec3 halfExtents,
                                                        const NavigationPathRequest &request, const NavigationAreaRegistry &areaRegistry,
                                                        const std::vector<GroundedNavigationPolygon> &polygons,
                                                        const std::vector<dtPolyRef> &polygonReferences);

    /** @brief Calculates the weighted cost of rooted, bounded corridor legs. */
    [[nodiscard]] Result<float> CalculatePathCost(const PathBuildContext &context, std::uint32_t count, Math::Vec3 destination);

    /** @brief Drops trailing traversal portals whose boundary pair did not fit. */
    void TrimUnpairedTraversal(NavigationPath &path, std::uint32_t coveredPolygons);

    [[nodiscard]] Result<NavigationPath> BuildPath(PathBuildContext &context);
}  // namespace Horo::Navigation::RecastDetourQueries
