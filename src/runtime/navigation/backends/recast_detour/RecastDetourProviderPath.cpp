#include "runtime/navigation/backends/recast_detour/RecastDetourProviderQueriesInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Navigation::RecastDetourQueries {
    namespace {
        using Detail::Failure;

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

        struct StraightPathScratch final {
            int pointCount{};
            bool pointBudgetExceeded{};
        };

        struct PathGeometry final {
            Math::Vec3 effectiveTarget{};
            std::uint32_t costPolygonCount{};
            bool pointBudgetExceeded{};
        };

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

        [[nodiscard]] Result<StraightPathScratch> FindStraightPathPoints(PathBuildContext &context, const Math::Vec3 target) {
            for (std::uint32_t index = 0; index < context.search.polygonCount; ++index)
                context.slot.polygonPath[index] = context.references[context.slot.polygonPathIndices[index]];
            const auto scratchCapacity = static_cast<std::uint32_t>(context.slot.straightPoints.size() / 3U);
            const auto boundedPoints = static_cast<int>(std::min(context.request.requirement.limits.maximumResultPoints, scratchCapacity));
            const std::array<float, 3> projectedStart{context.start.projected.x, context.start.projected.y, context.start.projected.z};
            const std::array<float, 3> projectedTarget{target.x, target.y, target.z};
            int pointCount{};
            const dtStatus straightStatus =
                context.slot.query->findStraightPath(projectedStart.data(), projectedTarget.data(), context.slot.polygonPath.data(),
                                                     static_cast<int>(context.search.polygonCount), context.slot.straightPoints.data(),
                                                     context.slot.straightFlags.data(), context.slot.straightPolygons.data(), &pointCount,
                                                     boundedPoints);
            if (pointCount < 0 || pointCount > boundedPoints)
                return Failure<StraightPathScratch>(NavigationErrors::ProviderFailed);
            const bool reachedTarget = pointCount > 0 && (context.slot.straightFlags[pointCount - 1] & DT_STRAIGHTPATH_END) != 0;
            const bool pointBudgetExceeded = dtStatusDetail(straightStatus, DT_BUFFER_TOO_SMALL) && !reachedTarget;
            if (dtStatusFailed(straightStatus) && !pointBudgetExceeded)
                return Failure<StraightPathScratch>(NavigationErrors::ProviderFailed);
            if (pointBudgetExceeded && context.request.coveragePolicy == NavigationPathCoveragePolicy::RequireComplete)
                return Failure<StraightPathScratch>(NavigationErrors::CapacityExceeded);
            return Result<StraightPathScratch>::Success({.pointCount = pointCount, .pointBudgetExceeded = pointBudgetExceeded});
        }

        [[nodiscard]] Result<PathGeometry> PopulatePathPoints(PathBuildContext &context, NavigationPath &path, const Math::Vec3 target) {
            const auto scratch = FindStraightPathPoints(context, target);
            if (scratch.HasError())
                return Result<PathGeometry>::Failure(scratch.ErrorValue());
            try {
                path.points.reserve(static_cast<std::size_t>(std::max(scratch.Value().pointCount, 0)));
                for (int index = 0; index < scratch.Value().pointCount; ++index) {
                    const std::size_t offset = static_cast<std::size_t>(index) * 3U;
                    path.points.push_back({context.slot.straightPoints[offset], context.slot.straightPoints[offset + 1U],
                                           context.slot.straightPoints[offset + 2U]});
                }
                if (path.points.empty())
                    return Failure<PathGeometry>(NavigationErrors::ProviderFailed);
                path.points.front() = context.request.start;
                PathGeometry geometry{.effectiveTarget = target,
                                      .costPolygonCount = context.search.polygonCount,
                                      .pointBudgetExceeded = scratch.Value().pointBudgetExceeded};
                if (!geometry.pointBudgetExceeded)
                    path.points.back() = target;
                else {
                    path.status = NavigationPathStatus::Partial;
                    path.stopReason = NavigationPathStopReason::ResultPointBudgetExceeded;
                    geometry.effectiveTarget = path.points.back();
                    path.stopPosition = geometry.effectiveTarget;
                    const dtPolyRef frontierReference = context.slot.straightPolygons[scratch.Value().pointCount - 1];
                    std::uint32_t frontierPosition = InvalidNavigationPolygonIndex;
                    for (std::uint32_t index = 0; index < context.search.polygonCount; ++index) {
                        if (context.references[context.slot.polygonPathIndices[index]] == frontierReference) {
                            frontierPosition = index;
                            break;
                        }
                    }
                    if (frontierPosition == InvalidNavigationPolygonIndex)
                        return Failure<PathGeometry>(NavigationErrors::ProviderFailed);
                    path.stopPolygonIndex = context.slot.polygonPathIndices[frontierPosition];
                    geometry.costPolygonCount = frontierPosition + 1U;
                }
                return Result<PathGeometry>::Success(geometry);
            } catch (const std::bad_alloc &) {
                return Failure<PathGeometry>(NavigationErrors::CapacityExceeded);
            }
        }

        [[nodiscard]] Result<void> PopulatePathMetrics(PathBuildContext &context, NavigationPath &path, const PathGeometry &geometry) {
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
        const auto target = ResolvePathTarget(context, path);
        if (target.HasError())
            return Result<NavigationPath>::Failure(target.ErrorValue());
        const auto geometry = PopulatePathPoints(context, path, target.Value());
        if (geometry.HasError())
            return Result<NavigationPath>::Failure(geometry.ErrorValue());
        if (context.search.status == NavigationPathStatus::Reachable && !geometry.Value().pointBudgetExceeded) {
            path.status = NavigationPathStatus::Reachable;
            path.stopReason = NavigationPathStopReason::None;
            path.stopPosition = context.request.destination;
        }
        const auto metrics = PopulatePathMetrics(context, path, geometry.Value());
        if (metrics.HasError())
            return Result<NavigationPath>::Failure(metrics.ErrorValue());
        return Result<NavigationPath>::Success(std::move(path));
    }
}  // namespace Horo::Navigation::RecastDetourQueries
