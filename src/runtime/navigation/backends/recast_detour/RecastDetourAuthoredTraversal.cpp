#include "RecastDetourProviderQueriesInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Checks the projected link endpoint against its exact declared ground polygon. */
        [[nodiscard]] bool IsLinkEndpoint(const dtNavMeshQuery &query, const dtPolyRef polygon, const Math::Vec3 position) {
            const std::array point{position.x, position.y, position.z};
            std::array<float, 3> projected{};
            bool overPolygon{};
            return dtStatusSucceed(query.closestPointOnPoly(polygon, point.data(), projected.data(), &overPolygon)) && overPolygon &&
                   std::abs(projected[1] - position.y) <= 0.001F;
        }

        /** @brief Validates directed degree and prevents ambiguity with shared ground edges or another authored arc. */
        [[nodiscard]] bool ValidateLinkArcs(const std::span<const RecastDetourLinkArc> arcs,
                                            const std::span<const NavigationPolygonAdjacency> adjacency) {
            constexpr std::size_t MaximumLinksPerPolygon = 8;
            std::uint32_t previousFrom = InvalidNavigationPolygonIndex;
            std::uint32_t previousTo = InvalidNavigationPolygonIndex;
            std::size_t outgoing{};
            for (const auto &arc : arcs) {
                if (previousFrom != arc.from) {
                    previousFrom = arc.from;
                    previousTo = InvalidNavigationPolygonIndex;
                    outgoing = 0;
                }
                const auto &ground = adjacency[arc.from];
                ++outgoing;
                if (const auto neighbors = std::span{ground.neighbors}.first(ground.count);
                    previousTo == arc.to || outgoing > MaximumLinksPerPolygon || std::ranges::find(neighbors, arc.to) != neighbors.end())
                    return false;
                previousTo = arc.to;
            }
            return true;
        }

    }  // namespace

    /** @copydoc BuildLinkArcs */
    [[nodiscard]] Result<std::vector<RecastDetourLinkArc>> BuildLinkArcs(const NavigationTileBuildResult *cooked,
                                                                         const std::span<const NavigationPolygonAdjacency> adjacency,
                                                                         const dtNavMeshQuery &query,
                                                                         const std::span<const dtPolyRef> references,
                                                                         const NavigationAreaRegistry &areas) {
        using Arcs = Result<std::vector<RecastDetourLinkArc>>;
        std::vector<RecastDetourLinkArc> arcs;
        if (!cooked)
            return Arcs::Success(std::move(arcs));
        arcs.reserve(cooked->offMeshLinks.size() * 2);
        for (const auto &link : cooked->offMeshLinks) {
            if (link.startPolygon == link.endPolygon || !IsLinkEndpoint(query, references[link.startPolygon], link.start) ||
                !IsLinkEndpoint(query, references[link.endPolygon], link.end))
                return Arcs::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            if (const auto area = areas.ResolveArea(link.area); area.HasError())
                return Arcs::Failure(area.ErrorValue());
            arcs.emplace_back(link.startPolygon, link.endPolygon, link.authoredLink, link.area, link.start, link.end, link.radiusMeters);
            if (link.bidirectional)
                arcs.emplace_back(link.endPolygon, link.startPolygon, link.authoredLink, link.area, link.end, link.start,
                                  link.radiusMeters);
        }
        std::ranges::sort(arcs, std::less<>{}, &RecastDetourLinkArc::Key);
        if (!ValidateLinkArcs(arcs, adjacency))
            return Arcs::Failure(MakeError(NavigationErrors::OperationUnsupported, "Ambiguous or excessive authored link arcs."));
        return Arcs::Success(std::move(arcs));
    }

    /** @copydoc FindRecastDetourLinkArc */
    const RecastDetourLinkArc *FindRecastDetourLinkArc(const std::span<const RecastDetourLinkArc> arcs, const std::uint32_t from,
                                                       const std::uint32_t to) {
        const std::pair key{from, to};
        const auto arc = std::ranges::lower_bound(arcs, key, std::less<>{}, &RecastDetourLinkArc::Key);
        return arc != arcs.end() && arc->Key() == key ? std::to_address(arc) : nullptr;
    }

    /** @copydoc ResolveTraversalEdge */
    Result<NavigationTraversalPolicy> ResolveTraversalEdge(const NavigationAreaRegistry &areas, const NavigationFilterId filter,
                                                           const NavigationAreaId destinationArea, const RecastDetourLinkArc *arc,
                                                           const float requestedClearance, const float defaultClearance) {
        if (auto destination = areas.ResolveTraversal(filter, destinationArea);
            !arc || destination.HasError() || !destination.Value().traversable)
            return destination;
        if (const float clearance = requestedClearance == 0 ? defaultClearance : requestedClearance; clearance > arc->radiusMeters)
            return Result<NavigationTraversalPolicy>::Success({.traversable = false});
        return areas.ResolveTraversal(filter, arc->area);
    }

    /** @copydoc SelectAuthoredTraversalFrontier */
    std::uint32_t SelectAuthoredTraversalFrontier(const std::span<const NavigationAStarNode> nodes,
                                                  const std::span<const Math::Vec3> centers, const Math::Vec3 target) {
        const auto best = std::ranges::min_element(nodes, std::less<>{}, [&](const NavigationAStarNode &node) {
            const auto point = centers[node.polygon];
            const auto distance = std::hypot(static_cast<double>(point.x) - target.x, static_cast<double>(point.y) - target.y,
                                             static_cast<double>(point.z) - target.z);
            return std::tuple{distance, node.cost, node.polygon};
        });
        return static_cast<std::uint32_t>(best - nodes.begin());
    }
}  // namespace Horo::Navigation

namespace Horo::Navigation::RecastDetourQueries {
    using Detail::Failure;

    namespace {
        /** @brief Resolves one area's declared travel cost and multiplies by a finite leg distance. */
        [[nodiscard]] Result<double> WeightedLegCost(const PathBuildContext &context, const NavigationAreaId area, const Math::Vec3 from,
                                                     const Math::Vec3 to) {
            const auto policy = context.areaRegistry.ResolveTraversal(context.request.filter, area);
            if (policy.HasError())
                return Result<double>::Failure(policy.ErrorValue());
            const auto distance = Detail::PointDistance(from, to);
            if (distance.HasError())
                return Result<double>::Failure(distance.ErrorValue());
            return Result<double>::Success(static_cast<double>(distance.Value()) * policy.Value().traversalCost);
        }
    }  // namespace

    /** @copydoc CalculatePathCost */
    Result<float> CalculatePathCost(const PathBuildContext &context, const std::uint32_t count, const Math::Vec3 destination) {
        using Cost = Result<float>;
        if (count == 0)
            return Cost::Success(0.0F);
        const auto &pathIndices = context.slot.polygonPathIndices;
        const auto &polygons = context.polygons;
        const auto &centers = context.centers;
        const auto start = pathIndices.front();
        const auto firstLeg = WeightedLegCost(context, polygons[start].area, context.start.projected, centers[start]);
        if (firstLeg.HasError())
            return Cost::Failure(firstLeg.ErrorValue());
        double total{};
        total += firstLeg.Value();
        for (std::uint32_t index = 1; index < count; ++index) {
            const auto from = pathIndices[index - 1U];
            const auto to = pathIndices[index];
            const auto *link = FindRecastDetourLinkArc(context.links, from, to);
            const auto leg = WeightedLegCost(context, link ? link->area : polygons[to].area, centers[from], centers[to]);
            if (leg.HasError())
                return Cost::Failure(leg.ErrorValue());
            total += leg.Value();
        }
        const auto last = pathIndices[count - 1U];
        const auto lastLeg = WeightedLegCost(context, polygons[last].area, centers[last], destination);
        if (lastLeg.HasError())
            return Cost::Failure(lastLeg.ErrorValue());
        total += lastLeg.Value();
        if (!std::isfinite(total) || total > std::numeric_limits<float>::max())
            return Failure<float>(NavigationErrors::QueryLimitExceeded);
        return Cost::Success(static_cast<float>(total));
    }

    /** @copydoc TrimUnpairedTraversal */
    void TrimUnpairedTraversal(NavigationPath &path, const std::uint32_t coveredPolygons) {
        const auto reachable = std::span{path.portals}.subspan(coveredPolygons - 1U);
        const auto unpaired = std::ranges::find(reachable, NavigationPathPortalKind::OffMeshLink, &NavigationPathPortal::kind);
        if (unpaired == reachable.end())
            return;
        const auto portal = coveredPolygons - 1U + static_cast<std::size_t>(unpaired - reachable.begin());
        if (auto &frontier = path.waypoints.back().provenance; frontier.portalIndex == portal) {
            // A corner on the synthetic target can stop at the entrance before the pair fits.
            frontier.kind = NavigationPathWaypointKind::PartialStop;
            frontier.portalIndex = NavigationPathNoPortal;
            frontier.vertexIndex = NavigationPathNoVertex;
        }
        path.portals.resize(portal);
        path.corridor.resize(portal + 1);
    }
}  // namespace Horo::Navigation::RecastDetourQueries
