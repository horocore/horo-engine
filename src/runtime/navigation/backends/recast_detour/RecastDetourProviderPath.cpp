#include "runtime/navigation/backends/recast_detour/RecastDetourProviderQueriesInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::Navigation::RecastDetourQueries {
    namespace {
        using Detail::Failure;

        constexpr double FunnelEpsilon = 1.0e-5;
        constexpr double PortalLengthEpsilon = 1.0e-5;

        [[nodiscard]] double CrossXZ(const Math::Vec3 first, const Math::Vec3 second) noexcept {
            return (static_cast<double>(first.x) * second.z) - (static_cast<double>(first.z) * second.x);
        }

        [[nodiscard]] double TriangleAreaXZ(const Math::Vec3 apex, const Math::Vec3 first, const Math::Vec3 second) noexcept {
            return CrossXZ(first - apex, second - apex);
        }

        [[nodiscard]] bool SamePoint(const Math::Vec3 first, const Math::Vec3 second) noexcept {
            return std::abs(static_cast<double>(first.x) - second.x) <= FunnelEpsilon &&
                   std::abs(static_cast<double>(first.y) - second.y) <= FunnelEpsilon &&
                   std::abs(static_cast<double>(first.z) - second.z) <= FunnelEpsilon;
        }

        [[nodiscard]] std::uint32_t MaximumCorridorPolygons(const PathBuildContext &context) noexcept {
            const std::uint32_t requested = context.request.outputLimits.maximumCorridorPolygons;
            const std::uint32_t prepared = static_cast<std::uint32_t>(context.slot.polygonPathIndices.size());
            const std::uint32_t admitted = context.request.requirement.limits.maximumNodeExpansions;
            return std::min(prepared, requested == 0 ? admitted : requested);
        }

        [[nodiscard]] std::uint32_t MaximumPortals(const PathBuildContext &context) noexcept {
            const std::uint32_t requested = context.request.outputLimits.maximumPortals;
            const std::uint32_t prepared = static_cast<std::uint32_t>(context.slot.portals.capacity());
            const std::uint32_t admitted = context.request.requirement.limits.maximumResultPoints;
            return std::min(prepared, requested == 0 ? admitted : requested);
        }

        [[nodiscard]] std::uint32_t MaximumWaypoints(const PathBuildContext &context) noexcept {
            const std::uint32_t requested = context.request.outputLimits.maximumWaypoints;
            const std::uint32_t prepared = static_cast<std::uint32_t>(context.slot.waypoints.capacity());
            const std::uint32_t admitted = context.request.requirement.limits.maximumResultPoints;
            return std::min(prepared, requested == 0 ? admitted : requested);
        }

        [[nodiscard]] Result<float> PathLength(const std::vector<Math::Vec3> &points) {
            double length{};
            for (std::size_t index = 1; index < points.size(); ++index) {
                const Math::Vec3 &current = points[index];
                const Math::Vec3 &previous = points[index - 1U];
                length += std::hypot(static_cast<double>(current.x) - previous.x, static_cast<double>(current.y) - previous.y,
                                     static_cast<double>(current.z) - previous.z);
            }
            if (!std::isfinite(length) || length > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::ProviderFailed);
            return Result<float>::Success(static_cast<float>(length));
        }

        [[nodiscard]] Result<Math::Vec3> PartialTarget(QuerySlot &slot, const std::uint32_t polygon, const Math::Vec3 destination,
                                                       const dtPolyRef reference) {
            if (polygon == InvalidNavigationPolygonIndex)
                return Result<Math::Vec3>::Success(destination);
            const std::array<float, 3> source{destination.x, destination.y, destination.z};
            std::array<float, 3> projected{};
            bool pointOverPolygon{};
            if (dtStatusFailed(slot.query->closestPointOnPoly(reference, source.data(), projected.data(), &pointOverPolygon)))
                return Failure<Math::Vec3>(NavigationErrors::ProviderFailed);
            if (!std::ranges::all_of(projected, [](const float value) {
                return std::isfinite(value);
            }))
                return Failure<Math::Vec3>(NavigationErrors::ProviderFailed);
            return Result<Math::Vec3>::Success({projected[0], projected[1], projected[2]});
        }

        [[nodiscard]] Result<float> CalculatePathCost(const std::vector<std::uint32_t> &pathIndices, const std::uint32_t count,
                                                      const Math::Vec3 start, const Math::Vec3 destination,
                                                      const NavigationAreaRegistry &areaRegistry, const NavigationFilterId filter,
                                                      const std::vector<GroundedNavigationPolygon> &polygons,
                                                      const std::vector<Math::Vec3> &centers) {
            if (count == 0)
                return Result<float>::Success(0.0F);
            double cost{};
            const auto firstPolicy = areaRegistry.ResolveTraversal(filter, polygons[pathIndices[0]].area);
            if (firstPolicy.HasError())
                return Result<float>::Failure(firstPolicy.ErrorValue());
            auto firstDistance = Detail::PointDistance(start, centers[pathIndices[0]]);
            if (firstDistance.HasError())
                return firstDistance;
            cost += static_cast<double>(firstDistance.Value()) * firstPolicy.Value().traversalCost;
            for (std::uint32_t index = 1; index < count; ++index) {
                const auto policy = areaRegistry.ResolveTraversal(filter, polygons[pathIndices[index]].area);
                if (policy.HasError())
                    return Result<float>::Failure(policy.ErrorValue());
                const auto distance = Detail::PointDistance(centers[pathIndices[index - 1U]], centers[pathIndices[index]]);
                if (distance.HasError())
                    return distance;
                cost += static_cast<double>(distance.Value()) * policy.Value().traversalCost;
            }
            const auto lastPolicy = areaRegistry.ResolveTraversal(filter, polygons[pathIndices[count - 1U]].area);
            if (lastPolicy.HasError())
                return Result<float>::Failure(lastPolicy.ErrorValue());
            const auto lastDistance = Detail::PointDistance(centers[pathIndices[count - 1U]], destination);
            if (lastDistance.HasError())
                return lastDistance;
            cost += static_cast<double>(lastDistance.Value()) * lastPolicy.Value().traversalCost;
            if (!std::isfinite(cost) || cost > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::QueryLimitExceeded);
            return Result<float>::Success(static_cast<float>(cost));
        }

        struct SharedEdge final {
            std::uint32_t first{};
            std::uint32_t second{};
        };

        [[nodiscard]] bool IsSameUndirectedEdge(const std::uint32_t first, const std::uint32_t second, const std::uint32_t otherFirst,
                                                const std::uint32_t otherSecond) noexcept {
            return (first == otherFirst && second == otherSecond) || (first == otherSecond && second == otherFirst);
        }

        [[nodiscard]] Result<SharedEdge> FindSharedEdge(const GroundedNavigationPolygon &from, const GroundedNavigationPolygon &to) {
            std::optional<SharedEdge> result;
            for (std::uint8_t fromEdge = 0; fromEdge < from.vertexCount; ++fromEdge) {
                const std::uint32_t fromFirst = from.vertexIndices[fromEdge];
                const std::uint32_t fromSecond = from.vertexIndices[(fromEdge + 1U) % from.vertexCount];
                for (std::uint8_t toEdge = 0; toEdge < to.vertexCount; ++toEdge) {
                    const std::uint32_t toFirst = to.vertexIndices[toEdge];
                    const std::uint32_t toSecond = to.vertexIndices[(toEdge + 1U) % to.vertexCount];
                    if (!IsSameUndirectedEdge(fromFirst, fromSecond, toFirst, toSecond))
                        continue;
                    if (result.has_value())
                        return Failure<SharedEdge>(NavigationErrors::PathPortalDegenerate);
                    result = SharedEdge{.first = fromFirst, .second = fromSecond};
                }
            }
            if (!result.has_value())
                return Failure<SharedEdge>(NavigationErrors::PathPortalDegenerate);
            return Result<SharedEdge>::Success(*result);
        }

        struct OrderedPortal final {
            Math::Vec3 left{};
            Math::Vec3 right{};
            std::uint32_t leftVertex{};
            std::uint32_t rightVertex{};
            double rawLength{};
        };

        [[nodiscard]] Result<OrderedPortal> MakeOrderedPortal(const PathBuildContext &context, const std::uint32_t fromIndex,
                                                              const std::uint32_t toIndex, const SharedEdge shared) {
            if (shared.first >= context.vertices.size() || shared.second >= context.vertices.size())
                return Failure<OrderedPortal>(NavigationErrors::ProviderFailed);
            const Math::Vec3 first = context.vertices[shared.first];
            const Math::Vec3 second = context.vertices[shared.second];
            if (!Math::IsFinite(first) || !Math::IsFinite(second))
                return Failure<OrderedPortal>(NavigationErrors::ProviderFailed);
            const double rawLength = std::hypot(static_cast<double>(second.x) - first.x, static_cast<double>(second.y) - first.y,
                                                static_cast<double>(second.z) - first.z);
            if (!std::isfinite(rawLength) || rawLength <= PortalLengthEpsilon)
                return Failure<OrderedPortal>(NavigationErrors::PathPortalDegenerate);

            Math::Vec3 travel = context.centers[toIndex] - context.centers[fromIndex];
            if (std::hypot(static_cast<double>(travel.x), static_cast<double>(travel.z)) <= FunnelEpsilon)
                travel = context.request.destination - context.request.start;
            const double travelLength = std::hypot(static_cast<double>(travel.x), static_cast<double>(travel.z));
            const double orientation = CrossXZ(travel, second - first);
            if (!std::isfinite(travelLength) || travelLength <= FunnelEpsilon || !std::isfinite(orientation) ||
                std::abs(orientation) <= FunnelEpsilon)
                return Failure<OrderedPortal>(NavigationErrors::PathPortalDegenerate);
            const bool reverse = orientation < 0.0;
            return Result<OrderedPortal>::Success({.left = reverse ? first : second,
                                                   .right = reverse ? second : first,
                                                   .leftVertex = reverse ? shared.first : shared.second,
                                                   .rightVertex = reverse ? shared.second : shared.first,
                                                   .rawLength = rawLength});
        }

        [[nodiscard]] Result<NavigationPathPortal> MakePortal(const PathBuildContext &context, const std::uint32_t corridorIndex) {
            if (corridorIndex + 1U >= context.search.polygonCount)
                return Failure<NavigationPathPortal>(NavigationErrors::ProviderFailed);
            const std::uint32_t fromIndex = context.slot.polygonPathIndices[corridorIndex];
            const std::uint32_t toIndex = context.slot.polygonPathIndices[corridorIndex + 1U];
            if (fromIndex >= context.polygons.size() || toIndex >= context.polygons.size())
                return Failure<NavigationPathPortal>(NavigationErrors::ProviderFailed);
            const GroundedNavigationPolygon &from = context.polygons[fromIndex];
            const GroundedNavigationPolygon &to = context.polygons[toIndex];
            const auto shared = FindSharedEdge(from, to);
            if (shared.HasError())
                return Result<NavigationPathPortal>::Failure(shared.ErrorValue());
            const auto geometry = MakeOrderedPortal(context, fromIndex, toIndex, shared.Value());
            if (geometry.HasError())
                return Result<NavigationPathPortal>::Failure(geometry.ErrorValue());
            const float clearance =
                context.request.clearanceMeters == 0.0F ? context.defaultClearanceMeters : context.request.clearanceMeters;
            if (!std::isfinite(clearance) || clearance < 0.0F)
                return Failure<NavigationPathPortal>(NavigationErrors::CapabilityDescriptorInvalid);
            if (static_cast<double>(clearance) * 2.0 + PortalLengthEpsilon >= geometry.Value().rawLength)
                return Failure<NavigationPathPortal>(NavigationErrors::PathPortalDegenerate);

            const float ratio = static_cast<float>(static_cast<double>(clearance) / geometry.Value().rawLength);
            const Math::Vec3 edge = geometry.Value().right - geometry.Value().left;
            const Math::Vec3 left = geometry.Value().left + edge * ratio;
            const Math::Vec3 right = geometry.Value().right - edge * ratio;
            const double width = std::hypot(static_cast<double>(right.x) - left.x, static_cast<double>(right.y) - left.y,
                                            static_cast<double>(right.z) - left.z);
            if (!Math::IsFinite(left) || !Math::IsFinite(right) || !std::isfinite(width) || width <= PortalLengthEpsilon)
                return Failure<NavigationPathPortal>(NavigationErrors::PathPortalDegenerate);
            return Result<NavigationPathPortal>::Success({.kind = NavigationPathPortalKind::SharedPolygonEdge,
                                                          .fromPolygonIndex = corridorIndex,
                                                          .toPolygonIndex = corridorIndex + 1U,
                                                          .leftVertexIndex = geometry.Value().leftVertex,
                                                          .rightVertexIndex = geometry.Value().rightVertex,
                                                          .left = left,
                                                          .right = right,
                                                          .widthMeters = static_cast<float>(width),
                                                          .clearanceMeters = clearance});
        }

        [[nodiscard]] Result<void> BuildCorridorAndPortals(PathBuildContext &context, NavigationPath &path) {
            const std::uint32_t corridorCount = context.search.polygonCount;
            if (corridorCount == 0 || corridorCount > MaximumCorridorPolygons(context))
                return Failure<void>(NavigationErrors::CapacityExceeded);
            if (corridorCount > context.slot.polygonPathIndices.size())
                return Failure<void>(NavigationErrors::ProviderFailed);
            try {
                path.corridor.reserve(corridorCount);
                for (std::uint32_t index = 0; index < corridorCount; ++index) {
                    const std::uint32_t polygonIndex = context.slot.polygonPathIndices[index];
                    if (polygonIndex >= context.polygons.size())
                        return Failure<void>(NavigationErrors::ProviderFailed);
                    path.corridor.push_back({.provenance = {.world = context.request.world,
                                                            .topology = context.request.topology,
                                                            .surface = context.polygons[polygonIndex].surface,
                                                            .polygonIndex = polygonIndex},
                                             .area = context.polygons[polygonIndex].area});
                }

                context.slot.portals.clear();
                const std::uint32_t portalCount = corridorCount - 1U;
                if (portalCount > MaximumPortals(context) || portalCount > context.slot.portals.capacity())
                    return Failure<void>(NavigationErrors::CapacityExceeded);
                for (std::uint32_t index = 0; index < portalCount; ++index) {
                    const auto portal = MakePortal(context, index);
                    if (portal.HasError())
                        return Result<void>::Failure(portal.ErrorValue());
                    context.slot.portals.push_back(portal.Value());
                }
                path.portals.assign(context.slot.portals.begin(), context.slot.portals.end());
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                return Failure<void>(NavigationErrors::CapacityExceeded);
            }
        }

        [[nodiscard]] NavigationPath MakePathSkeleton(const PathBuildContext &context) {
            NavigationPath path;
            path.status = context.search.status;
            path.stopReason = context.search.stopReason;
            path.sourceGeneration = context.request.topology;
            path.stopPolygonIndex = context.search.terminalNode == InvalidNavigationPolygonIndex
                                        ? InvalidNavigationPolygonIndex
                                        : context.slot.searchNodes[context.search.terminalNode].polygon;
            path.stopPosition = context.search.status == NavigationPathStatus::Reachable
                                    ? context.request.destination
                                    : (path.stopPolygonIndex == InvalidNavigationPolygonIndex ? context.request.start
                                                                                              : context.centers[path.stopPolygonIndex]);
            return path;
        }

        [[nodiscard]] Result<Math::Vec3> ResolvePathTarget(PathBuildContext &context, NavigationPath &path) {
            if (context.search.status == NavigationPathStatus::Reachable)
                return Result<Math::Vec3>::Success(context.request.destination);
            auto partialTarget =
                PartialTarget(context.slot, path.stopPolygonIndex, context.request.destination, context.references[path.stopPolygonIndex]);
            if (partialTarget.HasError())
                return Result<Math::Vec3>::Failure(partialTarget.ErrorValue());
            path.stopPosition = partialTarget.Value();
            return partialTarget;
        }

        [[nodiscard]] NavigationPathWaypoint MakeWaypoint(const NavigationPath &path, const Math::Vec3 position,
                                                          const NavigationPathWaypointKind kind, const std::uint32_t corridorIndex,
                                                          const std::uint32_t portalIndex, const std::uint32_t vertexIndex) {
            const std::uint32_t boundedCorridorIndex = path.corridor.empty()
                                                           ? NavigationPathNoPolygon
                                                           : std::min(corridorIndex, static_cast<std::uint32_t>(path.corridor.size() - 1U));
            return {.position = position,
                    .provenance = {.kind = kind,
                                   .polygon = boundedCorridorIndex == NavigationPathNoPolygon
                                                  ? NavigationQueryProvenance{}
                                                  : path.corridor[boundedCorridorIndex].provenance,
                                   .corridorPolygonIndex = boundedCorridorIndex,
                                   .portalIndex = portalIndex,
                                   .vertexIndex = vertexIndex}};
        }

        enum class WaypointAppendResult : std::uint8_t {
            Added,
            Duplicate,
            BudgetExceeded
        };

        [[nodiscard]] WaypointAppendResult AppendWaypoint(std::vector<NavigationPathWaypoint> &waypoints,
                                                          const NavigationPathWaypoint &waypoint, const std::uint32_t maximumWaypoints) {
            if (!Math::IsFinite(waypoint.position))
                return WaypointAppendResult::BudgetExceeded;
            if (!waypoints.empty() && SamePoint(waypoints.back().position, waypoint.position))
                return WaypointAppendResult::Duplicate;
            if (waypoints.size() >= maximumWaypoints)
                return WaypointAppendResult::BudgetExceeded;
            waypoints.push_back(waypoint);
            return WaypointAppendResult::Added;
        }

        struct FunnelResult final {
            bool pointBudgetExceeded{};
            Math::Vec3 effectiveTarget{};
            std::uint32_t costPolygonCount{};
        };

        struct FunnelState final {
            Math::Vec3 apex{};
            Math::Vec3 left{};
            Math::Vec3 right{};
            std::uint32_t leftPortal{NavigationPathNoPortal};
            std::uint32_t rightPortal{NavigationPathNoPortal};
        };

        struct FunnelProgress final {
            bool pointBudgetExceeded{};
            bool restart{};
            std::uint32_t restartPortal{};
        };

        [[nodiscard]] WaypointAppendResult AppendFunnelCorner(PathBuildContext &context, const NavigationPath &path, FunnelState &funnel,
                                                              const std::uint32_t cornerPortal, const bool useLeft,
                                                              const std::uint32_t maximumWaypoints) {
            const Math::Vec3 corner = useLeft ? funnel.left : funnel.right;
            const std::uint32_t corridorIndex = cornerPortal == NavigationPathNoPortal ? 0U : cornerPortal;
            const std::uint32_t vertexIndex = cornerPortal < path.portals.size() ? (useLeft ? path.portals[cornerPortal].leftVertexIndex
                                                                                            : path.portals[cornerPortal].rightVertexIndex)
                                                                                 : NavigationPathNoVertex;
            const auto waypoint =
                MakeWaypoint(path, corner, NavigationPathWaypointKind::PortalCorner, corridorIndex, cornerPortal, vertexIndex);
            const auto result = AppendWaypoint(context.slot.waypoints, waypoint, maximumWaypoints);
            if (result == WaypointAppendResult::BudgetExceeded)
                return result;
            funnel.apex = corner;
            funnel.left = corner;
            funnel.right = corner;
            funnel.leftPortal = cornerPortal;
            funnel.rightPortal = cornerPortal;
            return result;
        }

        [[nodiscard]] FunnelProgress ProcessFunnelPortal(PathBuildContext &context, const NavigationPath &path, FunnelState &funnel,
                                                         const Math::Vec3 target, const std::uint32_t portalIndex,
                                                         const std::uint32_t maximumWaypoints) {
            const bool isTargetPortal = portalIndex == path.portals.size();
            const Math::Vec3 newLeft = isTargetPortal ? target : path.portals[portalIndex].left;
            const Math::Vec3 newRight = isTargetPortal ? target : path.portals[portalIndex].right;
            if (TriangleAreaXZ(funnel.apex, funnel.right, newRight) <= FunnelEpsilon) {
                if (SamePoint(funnel.apex, funnel.right) || TriangleAreaXZ(funnel.apex, funnel.left, newRight) > FunnelEpsilon) {
                    funnel.right = newRight;
                    funnel.rightPortal = isTargetPortal ? NavigationPathNoPortal : portalIndex;
                } else {
                    const std::uint32_t cornerPortal = funnel.leftPortal == NavigationPathNoPortal ? portalIndex : funnel.leftPortal;
                    if (AppendFunnelCorner(context, path, funnel, cornerPortal, true, maximumWaypoints) ==
                        WaypointAppendResult::BudgetExceeded)
                        return {.pointBudgetExceeded = true};
                    return {.restart = true, .restartPortal = cornerPortal};
                }
            }
            if (TriangleAreaXZ(funnel.apex, funnel.left, newLeft) >= -FunnelEpsilon) {
                if (SamePoint(funnel.apex, funnel.left) || TriangleAreaXZ(funnel.apex, funnel.right, newLeft) < -FunnelEpsilon) {
                    funnel.left = newLeft;
                    funnel.leftPortal = isTargetPortal ? NavigationPathNoPortal : portalIndex;
                } else {
                    const std::uint32_t cornerPortal = funnel.rightPortal == NavigationPathNoPortal ? portalIndex : funnel.rightPortal;
                    if (AppendFunnelCorner(context, path, funnel, cornerPortal, false, maximumWaypoints) ==
                        WaypointAppendResult::BudgetExceeded)
                        return {.pointBudgetExceeded = true};
                    return {.restart = true, .restartPortal = cornerPortal};
                }
            }
            return {};
        }

        [[nodiscard]] Result<void> PublishWaypoints(PathBuildContext &context, NavigationPath &path, const Math::Vec3 target,
                                                    const std::uint32_t maximumWaypoints, bool &pointBudgetExceeded) {
            if (!pointBudgetExceeded) {
                const NavigationPathWaypointKind targetKind = context.search.status == NavigationPathStatus::Reachable
                                                                  ? NavigationPathWaypointKind::Destination
                                                                  : NavigationPathWaypointKind::PartialStop;
                const auto destination = MakeWaypoint(path, target, targetKind, static_cast<std::uint32_t>(path.corridor.size() - 1U),
                                                      NavigationPathNoPortal, NavigationPathNoVertex);
                if (AppendWaypoint(context.slot.waypoints, destination, maximumWaypoints) == WaypointAppendResult::BudgetExceeded)
                    pointBudgetExceeded = true;
            }
            if (pointBudgetExceeded && context.request.coveragePolicy == NavigationPathCoveragePolicy::RequireComplete)
                return Failure<void>(NavigationErrors::CapacityExceeded);
            if (context.slot.waypoints.empty())
                return Failure<void>(NavigationErrors::ProviderFailed);
            try {
                path.waypoints.assign(context.slot.waypoints.begin(), context.slot.waypoints.end());
                path.points.reserve(path.waypoints.size());
                for (const NavigationPathWaypoint &waypoint : path.waypoints)
                    path.points.push_back(waypoint.position);
            } catch (const std::bad_alloc &) {
                return Failure<void>(NavigationErrors::CapacityExceeded);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<FunnelResult> BuildWaypoints(PathBuildContext &context, NavigationPath &path, const Math::Vec3 target) {
            if (path.corridor.empty())
                return Failure<FunnelResult>(NavigationErrors::ProviderFailed);
            const std::uint32_t maximumWaypoints = MaximumWaypoints(context);
            if (maximumWaypoints < 2U)
                return Failure<FunnelResult>(NavigationErrors::CapacityExceeded);

            context.slot.waypoints.clear();
            const auto start = MakeWaypoint(path, context.request.start, NavigationPathWaypointKind::Start, 0, NavigationPathNoPortal,
                                            NavigationPathNoVertex);
            if (AppendWaypoint(context.slot.waypoints, start, maximumWaypoints) == WaypointAppendResult::BudgetExceeded)
                return Failure<FunnelResult>(NavigationErrors::CapacityExceeded);

            FunnelState funnel{.apex = context.start.projected, .left = context.start.projected, .right = context.start.projected};
            bool pointBudgetExceeded{};
            for (std::uint32_t portalIndex = 0; portalIndex <= path.portals.size(); ++portalIndex) {
                const FunnelProgress progress = ProcessFunnelPortal(context, path, funnel, target, portalIndex, maximumWaypoints);
                if (progress.pointBudgetExceeded) {
                    pointBudgetExceeded = true;
                    break;
                }
                if (progress.restart) {
                    portalIndex = progress.restartPortal;
                    continue;
                }
            }

            if (const auto publication = PublishWaypoints(context, path, target, maximumWaypoints, pointBudgetExceeded);
                publication.HasError())
                return Result<FunnelResult>::Failure(publication.ErrorValue());

            FunnelResult result{.pointBudgetExceeded = pointBudgetExceeded,
                                .effectiveTarget = path.points.back(),
                                .costPolygonCount = path.waypoints.back().provenance.corridorPolygonIndex + 1U};
            if (result.costPolygonCount == 0 || result.costPolygonCount > path.corridor.size())
                return Failure<FunnelResult>(NavigationErrors::ProviderFailed);
            return Result<FunnelResult>::Success(result);
        }

        [[nodiscard]] Result<void> PopulatePathMetrics(PathBuildContext &context, NavigationPath &path, const FunnelResult &geometry) {
            auto length = PathLength(path.points);
            if (length.HasError())
                return Result<void>::Failure(length.ErrorValue());
            path.lengthMeters = length.Value();
            auto cost = CalculatePathCost(context.slot.polygonPathIndices, geometry.costPolygonCount, context.start.projected,
                                          geometry.effectiveTarget, context.areaRegistry, context.request.filter, context.polygons,
                                          context.centers);
            if (cost.HasError())
                return Result<void>::Failure(cost.ErrorValue());
            path.cost = cost.Value();
            if (!std::isfinite(path.cost) || path.cost < 0.0F ||
                path.lengthMeters > context.request.requirement.limits.maximumSearchDistanceMeters)
                return Failure<void>(NavigationErrors::QueryLimitExceeded);
            if (!std::ranges::all_of(path.points, [](const Math::Vec3 point) {
                return Math::IsFinite(point);
            }))
                return Failure<void>(NavigationErrors::ProviderFailed);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BuildPath */
    Result<NavigationPath> BuildPath(PathBuildContext &context) {
        NavigationPath path = MakePathSkeleton(context);
        if (context.search.status != NavigationPathStatus::Reachable &&
            context.request.coveragePolicy == NavigationPathCoveragePolicy::RequireComplete)
            return Result<NavigationPath>::Success(std::move(path));
        if (context.search.polygonCount == 0 || context.search.terminalNode == InvalidNavigationPolygonIndex)
            return Result<NavigationPath>::Success(std::move(path));

        path.status = NavigationPathStatus::Partial;
        if (const auto corridor = BuildCorridorAndPortals(context, path); corridor.HasError())
            return Result<NavigationPath>::Failure(corridor.ErrorValue());
        const auto target = ResolvePathTarget(context, path);
        if (target.HasError())
            return Result<NavigationPath>::Failure(target.ErrorValue());
        const auto geometry = BuildWaypoints(context, path, target.Value());
        if (geometry.HasError())
            return Result<NavigationPath>::Failure(geometry.ErrorValue());
        if (geometry.Value().pointBudgetExceeded) {
            path.status = NavigationPathStatus::Partial;
            path.stopReason = NavigationPathStopReason::ResultPointBudgetExceeded;
            path.stopPosition = geometry.Value().effectiveTarget;
            path.stopPolygonIndex = path.waypoints.back().provenance.polygon.polygonIndex;
        } else if (context.search.status == NavigationPathStatus::Reachable) {
            path.status = NavigationPathStatus::Reachable;
            path.stopReason = NavigationPathStopReason::None;
            path.stopPosition = context.request.destination;
        }
        if (const auto metrics = PopulatePathMetrics(context, path, geometry.Value()); metrics.HasError())
            return Result<NavigationPath>::Failure(metrics.ErrorValue());
        return Result<NavigationPath>::Success(std::move(path));
    }
}  // namespace Horo::Navigation::RecastDetourQueries
