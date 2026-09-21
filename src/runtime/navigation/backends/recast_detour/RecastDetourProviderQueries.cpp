#include "Horo/Navigation/NavigationErrors.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using Detail::Failure;

        [[nodiscard]] Result<void> ValidateRequest(const NavigationPathRequest &request, const NavigationWorldId world,
                                                   const NavigationGeneration topology,
                                                   const NavigationProviderCapabilities &capabilities) {
            if (!request.world.IsValid() || request.world != world)
                return Failure<void>(NavigationErrors::InvalidWorld);
            if (!request.topology.IsValid() || request.topology != topology)
                return Failure<void>(NavigationErrors::StaleSnapshot);
            if (request.requirement.query != NavigationQueryKind::Path)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (!request.filter.IsValid() || request.coveragePolicy >= NavigationPathCoveragePolicy::Count)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (!Math::IsFinite(request.start) || !Math::IsFinite(request.destination))
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (request.requirement.limits.maximumResultPoints < 2U)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (const auto admitted = AdmitNavigationQuery(capabilities, capabilities.revision, request.requirement); admitted.HasError())
                return admitted;
            if (const double distance = std::hypot(static_cast<double>(request.destination.x) - request.start.x,
                                                   static_cast<double>(request.destination.y) - request.start.y,
                                                   static_cast<double>(request.destination.z) - request.start.z);
                !std::isfinite(distance) || distance > request.requirement.limits.maximumSearchDistanceMeters)
                return Failure<void>(NavigationErrors::QueryLimitExceeded);
            return Result<void>::Success();
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

        [[nodiscard]] Result<QueryEndpoint> ResolveEndpoint(const QuerySlot &slot, const Math::Vec3 point, const Math::Vec3 halfExtents,
                                                            const NavigationPathRequest &request,
                                                            const NavigationAreaRegistry &areaRegistry,
                                                            const std::vector<GroundedNavigationPolygon> &polygons,
                                                            const std::vector<dtPolyRef> &polygonReferences) {
            QueryEndpoint endpoint;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (std::size_t index = 0; index < polygons.size(); ++index) {
                const auto traversal = areaRegistry.ResolveTraversal(request.filter, polygons[index].area);
                if (traversal.HasError())
                    return Result<QueryEndpoint>::Failure(traversal.ErrorValue());
                if (!traversal.Value().traversable)
                    continue;

                const std::array<float, 3> source{point.x, point.y, point.z};
                std::array<float, 3> projected{};
                bool pointOverPolygon{};
                if (dtStatusFailed(
                        slot.query->closestPointOnPoly(polygonReferences[index], source.data(), projected.data(), &pointOverPolygon)))
                    return Failure<QueryEndpoint>(NavigationErrors::ProviderFailed);
                static_cast<void>(pointOverPolygon);
                if (!std::ranges::all_of(projected, [](const float value) {
                    return std::isfinite(value);
                }))
                    return Failure<QueryEndpoint>(NavigationErrors::ProviderFailed);
                if (std::abs(static_cast<double>(projected[0]) - point.x) > halfExtents.x ||
                    std::abs(static_cast<double>(projected[1]) - point.y) > halfExtents.y ||
                    std::abs(static_cast<double>(projected[2]) - point.z) > halfExtents.z)
                    continue;
                const double distance = std::hypot(static_cast<double>(projected[0]) - point.x, static_cast<double>(projected[1]) - point.y,
                                                   static_cast<double>(projected[2]) - point.z);
                if (!std::isfinite(distance) ||
                    (endpoint.found && (distance > bestDistance || (distance == bestDistance && index >= endpoint.polygon))))
                    continue;
                endpoint.found = true;
                endpoint.polygon = static_cast<std::uint32_t>(index);
                endpoint.projected = {projected[0], projected[1], projected[2]};
                bestDistance = distance;
            }
            return Result<QueryEndpoint>::Success(endpoint);
        }

        [[nodiscard]] std::uint32_t FindSearchNode(const QuerySlot &slot, const std::uint32_t nodeCount,
                                                   const std::uint32_t polygon) noexcept {
            for (std::uint32_t index = 0; index < nodeCount; ++index) {
                if (slot.searchNodes[index].polygon == polygon)
                    return index;
            }
            return InvalidNavigationPolygonIndex;
        }

        [[nodiscard]] bool IsBetterSearchNode(const NavigationAStarNode &candidate, const NavigationAStarNode &current) noexcept {
            return std::tuple{candidate.estimatedTotalCost, candidate.cost, candidate.polygon} <
                   std::tuple{current.estimatedTotalCost, current.cost, current.polygon};
        }

        [[nodiscard]] std::uint32_t SelectBestOpenNode(const QuerySlot &slot) noexcept {
            std::uint32_t best = InvalidNavigationPolygonIndex;
            for (const std::uint32_t nodeIndex : std::span{slot.openNodes.data(), slot.openNodes.size()}) {
                if (nodeIndex == InvalidNavigationPolygonIndex)
                    continue;
                const auto &candidate = slot.searchNodes[nodeIndex];
                if (!candidate.open)
                    continue;
                if (best == InvalidNavigationPolygonIndex) {
                    best = nodeIndex;
                    continue;
                }
                const auto &current = slot.searchNodes[best];
                if (IsBetterSearchNode(candidate, current))
                    best = nodeIndex;
            }
            return best;
        }

        [[nodiscard]] std::uint32_t SelectBestKnownNode(const QuerySlot &slot, const std::uint32_t nodeCount) noexcept {
            std::uint32_t best = InvalidNavigationPolygonIndex;
            for (std::uint32_t index = 0; index < nodeCount; ++index) {
                const auto &candidate = slot.searchNodes[index];
                if (candidate.polygon == InvalidNavigationPolygonIndex)
                    continue;
                if (best == InvalidNavigationPolygonIndex || IsBetterSearchNode(candidate, slot.searchNodes[best]))
                    best = index;
            }
            return best;
        }

        [[nodiscard]] Result<float> Heuristic(const Math::Vec3 from, const Math::Vec3 to, const float minimumTraversalCost) {
            const auto distance = Detail::PointDistance(from, to);
            if (distance.HasError())
                return distance;
            const double value = static_cast<double>(distance.Value()) * minimumTraversalCost;
            if (!std::isfinite(value) || value > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::QueryLimitExceeded);
            return Result<float>::Success(static_cast<float>(value));
        }

        [[nodiscard]] Result<float> MinimumTraversalCost(const NavigationAreaRegistry &areaRegistry, const NavigationFilterId filter,
                                                         const std::vector<GroundedNavigationPolygon> &polygons) {
            float minimum = std::numeric_limits<float>::infinity();
            for (const GroundedNavigationPolygon &polygon : polygons) {
                const auto traversal = areaRegistry.ResolveTraversal(filter, polygon.area);
                if (traversal.HasError())
                    return Result<float>::Failure(traversal.ErrorValue());
                if (traversal.Value().traversable)
                    minimum = std::min(minimum, traversal.Value().traversalCost);
            }
            return Result<float>::Success(std::isfinite(minimum) ? minimum : 0.0F);
        }

        [[nodiscard]] Result<CorridorSearch> FindCorridor(QuerySlot &slot, const QueryEndpoint &start, const QueryEndpoint &destination,
                                                          const NavigationPathRequest &request, const NavigationAreaRegistry &areaRegistry,
                                                          const std::vector<GroundedNavigationPolygon> &polygons,
                                                          const std::vector<NavigationPolygonAdjacency> &adjacency,
                                                          const std::vector<Math::Vec3> &centers, const CancellationToken &cancellation) {
            slot.openNodes.clear();
            const std::uint32_t nodeLimit =
                std::min({request.requirement.limits.maximumNodeExpansions, static_cast<std::uint32_t>(slot.searchNodes.size()),
                          static_cast<std::uint32_t>(slot.openNodes.capacity())});
            if (nodeLimit == 0)
                return Failure<CorridorSearch>(NavigationErrors::CapacityExceeded);
            for (std::uint32_t index = 0; index < nodeLimit; ++index)
                slot.searchNodes[index] = {};

            const auto minimumCost = MinimumTraversalCost(areaRegistry, request.filter, polygons);
            if (minimumCost.HasError())
                return Result<CorridorSearch>::Failure(minimumCost.ErrorValue());
            const auto startHeuristic = Heuristic(centers[start.polygon], centers[destination.polygon], minimumCost.Value());
            if (startHeuristic.HasError())
                return Result<CorridorSearch>::Failure(startHeuristic.ErrorValue());

            slot.searchNodes[0] = {.polygon = start.polygon,
                                   .parent = InvalidNavigationPolygonIndex,
                                   .cost = 0.0F,
                                   .estimatedTotalCost = startHeuristic.Value(),
                                   .open = true,
                                   .closed = false};
            slot.openNodes.push_back(0);
            std::uint32_t nodeCount = 1;
            std::uint32_t expansions{};
            std::uint32_t bestNode = 0;
            NavigationPathStatus terminalStatus{NavigationPathStatus::Unreachable};
            NavigationPathStopReason terminalReason{NavigationPathStopReason::DestinationUnreachable};
            bool nodeBudgetExhausted{};

            while (true) {
                if (cancellation.IsCancellationRequested())
                    return Failure<CorridorSearch>(NavigationErrors::QueryCancelled);
                const std::uint32_t currentIndex = SelectBestOpenNode(slot);
                if (currentIndex == InvalidNavigationPolygonIndex)
                    break;
                NavigationAStarNode &current = slot.searchNodes[currentIndex];
                if (current.polygon == destination.polygon) {
                    bestNode = currentIndex;
                    terminalStatus = NavigationPathStatus::Reachable;
                    terminalReason = NavigationPathStopReason::None;
                    break;
                }
                if (expansions >= request.requirement.limits.maximumNodeExpansions) {
                    bestNode = currentIndex;
                    terminalStatus = NavigationPathStatus::BudgetExceeded;
                    terminalReason = NavigationPathStopReason::NodeBudgetExceeded;
                    break;
                }
                current.open = false;
                current.closed = true;
                ++expansions;
                bestNode = currentIndex;

                const NavigationPolygonAdjacency &neighbors = adjacency[current.polygon];
                for (std::uint8_t neighborIndex = 0; neighborIndex < neighbors.count; ++neighborIndex) {
                    if (cancellation.IsCancellationRequested())
                        return Failure<CorridorSearch>(NavigationErrors::QueryCancelled);
                    const std::uint32_t neighbor = neighbors.neighbors[neighborIndex];
                    const auto traversal = areaRegistry.ResolveTraversal(request.filter, polygons[neighbor].area);
                    if (traversal.HasError())
                        return Result<CorridorSearch>::Failure(traversal.ErrorValue());
                    if (!traversal.Value().traversable)
                        continue;
                    const auto edgeDistance = Detail::PointDistance(centers[current.polygon], centers[neighbor]);
                    if (edgeDistance.HasError())
                        return Result<CorridorSearch>::Failure(edgeDistance.ErrorValue());
                    const double candidateCost =
                        static_cast<double>(current.cost) + static_cast<double>(edgeDistance.Value()) * traversal.Value().traversalCost;
                    if (!std::isfinite(candidateCost) || candidateCost > std::numeric_limits<float>::max())
                        return Failure<CorridorSearch>(NavigationErrors::QueryLimitExceeded);

                    std::uint32_t neighborNode = FindSearchNode(slot, nodeCount, neighbor);
                    if (neighborNode == InvalidNavigationPolygonIndex) {
                        if (nodeCount >= nodeLimit) {
                            nodeBudgetExhausted = true;
                            continue;
                        }
                        neighborNode = nodeCount++;
                        const auto estimate = Heuristic(centers[neighbor], centers[destination.polygon], minimumCost.Value());
                        if (estimate.HasError())
                            return Result<CorridorSearch>::Failure(estimate.ErrorValue());
                        const double estimatedTotalCost = candidateCost + static_cast<double>(estimate.Value());
                        if (!std::isfinite(estimatedTotalCost) || estimatedTotalCost > std::numeric_limits<float>::max())
                            return Failure<CorridorSearch>(NavigationErrors::QueryLimitExceeded);
                        slot.searchNodes[neighborNode] = {.polygon = neighbor,
                                                          .parent = currentIndex,
                                                          .cost = static_cast<float>(candidateCost),
                                                          .estimatedTotalCost = static_cast<float>(estimatedTotalCost),
                                                          .open = true,
                                                          .closed = false};
                        slot.openNodes.push_back(neighborNode);
                        continue;
                    }
                    NavigationAStarNode &existing = slot.searchNodes[neighborNode];
                    if (existing.closed && candidateCost >= existing.cost)
                        continue;
                    if (candidateCost > existing.cost ||
                        (candidateCost == existing.cost && existing.parent != InvalidNavigationPolygonIndex &&
                         current.polygon >= slot.searchNodes[existing.parent].polygon))
                        continue;
                    const auto estimate = Heuristic(centers[neighbor], centers[destination.polygon], minimumCost.Value());
                    if (estimate.HasError())
                        return Result<CorridorSearch>::Failure(estimate.ErrorValue());
                    const double estimatedTotalCost = candidateCost + static_cast<double>(estimate.Value());
                    if (!std::isfinite(estimatedTotalCost) || estimatedTotalCost > std::numeric_limits<float>::max())
                        return Failure<CorridorSearch>(NavigationErrors::QueryLimitExceeded);
                    existing.parent = currentIndex;
                    existing.cost = static_cast<float>(candidateCost);
                    existing.estimatedTotalCost = static_cast<float>(estimatedTotalCost);
                    existing.open = true;
                    existing.closed = false;
                }
            }

            CorridorSearch result{.status = NavigationPathStatus::Unreachable,
                                  .stopReason = NavigationPathStopReason::DestinationUnreachable,
                                  .terminalNode = bestNode};
            if (terminalStatus == NavigationPathStatus::Reachable || terminalStatus == NavigationPathStatus::BudgetExceeded) {
                result.status = terminalStatus;
                result.stopReason = terminalReason;
            } else if (nodeBudgetExhausted) {
                result.status = NavigationPathStatus::BudgetExceeded;
                result.stopReason = NavigationPathStopReason::NodeBudgetExceeded;
            }
            if (result.status != NavigationPathStatus::Reachable)
                bestNode = SelectBestKnownNode(slot, nodeCount);
            if (bestNode == InvalidNavigationPolygonIndex)
                result.terminalNode = InvalidNavigationPolygonIndex;
            else
                result.terminalNode = bestNode;

            std::uint32_t cursor = result.terminalNode;
            while (cursor != InvalidNavigationPolygonIndex && result.polygonCount < slot.polygonPathIndices.size()) {
                slot.polygonPathIndices[result.polygonCount++] = slot.searchNodes[cursor].polygon;
                cursor = slot.searchNodes[cursor].parent;
            }
            std::ranges::reverse(std::span{slot.polygonPathIndices.data(), result.polygonCount});
            return Result<CorridorSearch>::Success(result);
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

        [[nodiscard]] Result<NavigationPath> BuildPath(QuerySlot &slot, const QueryEndpoint &start, const NavigationPathRequest &request,
                                                       const CorridorSearch &search, const NavigationAreaRegistry &areaRegistry,
                                                       const std::vector<GroundedNavigationPolygon> &polygons,
                                                       const std::vector<Math::Vec3> &centers, const std::vector<dtPolyRef> &references) {
            NavigationPath path;
            path.status = search.status;
            path.stopReason = search.stopReason;
            path.sourceGeneration = request.topology;
            path.stopPolygonIndex = search.terminalNode == InvalidNavigationPolygonIndex ? InvalidNavigationPolygonIndex
                                                                                         : slot.searchNodes[search.terminalNode].polygon;
            path.stopPosition =
                search.status == NavigationPathStatus::Reachable
                    ? request.destination
                    : (path.stopPolygonIndex == InvalidNavigationPolygonIndex ? request.start : centers[path.stopPolygonIndex]);
            if (search.status != NavigationPathStatus::Reachable && request.coveragePolicy == NavigationPathCoveragePolicy::RequireComplete)
                return Result<NavigationPath>::Success(std::move(path));
            if (search.polygonCount == 0 || search.terminalNode == InvalidNavigationPolygonIndex)
                return Result<NavigationPath>::Success(std::move(path));

            path.status = NavigationPathStatus::Partial;

            Math::Vec3 target = request.destination;
            if (search.status != NavigationPathStatus::Reachable) {
                auto partialTarget = PartialTarget(slot, path.stopPolygonIndex, request.destination, references[path.stopPolygonIndex]);
                if (partialTarget.HasError())
                    return Result<NavigationPath>::Failure(partialTarget.ErrorValue());
                target = partialTarget.Value();
                path.stopPosition = target;
            }

            for (std::uint32_t index = 0; index < search.polygonCount; ++index)
                slot.polygonPath[index] = references[slot.polygonPathIndices[index]];
            int pointCount{};
            const auto scratchCapacity = static_cast<std::uint32_t>(slot.straightPoints.size() / 3U);
            const auto boundedPoints = static_cast<int>(std::min(request.requirement.limits.maximumResultPoints, scratchCapacity));
            const std::array<float, 3> projectedStart{start.projected.x, start.projected.y, start.projected.z};
            const std::array<float, 3> projectedTarget{target.x, target.y, target.z};
            const dtStatus straightStatus =
                slot.query->findStraightPath(projectedStart.data(), projectedTarget.data(), slot.polygonPath.data(),
                                             static_cast<int>(search.polygonCount), slot.straightPoints.data(), slot.straightFlags.data(),
                                             slot.straightPolygons.data(), &pointCount, boundedPoints);
            if (pointCount < 0 || pointCount > boundedPoints)
                return Failure<NavigationPath>(NavigationErrors::ProviderFailed);
            const bool reachedTarget = pointCount > 0 && (slot.straightFlags[pointCount - 1] & DT_STRAIGHTPATH_END) != 0;
            const bool pointBudgetExceeded = dtStatusDetail(straightStatus, DT_BUFFER_TOO_SMALL) && !reachedTarget;
            if (dtStatusFailed(straightStatus) && !pointBudgetExceeded)
                return Failure<NavigationPath>(NavigationErrors::ProviderFailed);
            if (pointBudgetExceeded) {
                if (request.coveragePolicy == NavigationPathCoveragePolicy::RequireComplete)
                    return Failure<NavigationPath>(NavigationErrors::CapacityExceeded);
                path.status = NavigationPathStatus::Partial;
                path.stopReason = NavigationPathStopReason::ResultPointBudgetExceeded;
            }
            try {
                path.points.reserve(static_cast<std::size_t>(std::max(pointCount, 0)));
                for (int index = 0; index < pointCount; ++index) {
                    const std::size_t offset = static_cast<std::size_t>(index) * 3U;
                    path.points.push_back(
                        {slot.straightPoints[offset], slot.straightPoints[offset + 1U], slot.straightPoints[offset + 2U]});
                }
                if (path.points.empty())
                    return Failure<NavigationPath>(NavigationErrors::ProviderFailed);
                path.points.front() = request.start;
                Math::Vec3 effectiveTarget = target;
                if (!pointBudgetExceeded)
                    path.points.back() = target;
                else
                    effectiveTarget = path.points.back();
                if (search.status == NavigationPathStatus::Reachable && !pointBudgetExceeded) {
                    path.status = NavigationPathStatus::Reachable;
                    path.stopReason = NavigationPathStopReason::None;
                    path.stopPosition = request.destination;
                }
                if (pointBudgetExceeded)
                    path.stopPosition = effectiveTarget;
                std::uint32_t costPolygonCount = search.polygonCount;
                if (pointBudgetExceeded) {
                    const dtPolyRef frontierReference = slot.straightPolygons[pointCount - 1];
                    std::uint32_t frontierPosition = InvalidNavigationPolygonIndex;
                    for (std::uint32_t index = 0; index < search.polygonCount; ++index) {
                        if (references[slot.polygonPathIndices[index]] == frontierReference) {
                            frontierPosition = index;
                            break;
                        }
                    }
                    if (frontierPosition == InvalidNavigationPolygonIndex)
                        return Failure<NavigationPath>(NavigationErrors::ProviderFailed);
                    path.stopPolygonIndex = slot.polygonPathIndices[frontierPosition];
                    costPolygonCount = frontierPosition + 1U;
                }
                auto length = PathLength(path.points);
                if (length.HasError())
                    return Result<NavigationPath>::Failure(length.ErrorValue());
                path.lengthMeters = length.Value();
                auto cost = CalculatePathCost(slot.polygonPathIndices, costPolygonCount, start.projected, effectiveTarget, areaRegistry,
                                              request.filter, polygons, centers);
                if (cost.HasError())
                    return Result<NavigationPath>::Failure(cost.ErrorValue());
                path.cost = cost.Value();
                if (!std::isfinite(path.cost) || path.cost < 0.0F ||
                    path.lengthMeters > request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationPath>(NavigationErrors::QueryLimitExceeded);
                return Result<NavigationPath>::Success(std::move(path));
            } catch (const std::bad_alloc &) {
                return Failure<NavigationPath>(NavigationErrors::CapacityExceeded);
            }
        }

        class RecastDetourNavigationQueryBackend final : public INavigationQueryBackend {
        public:
            RecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &info, NavMeshPtr mesh, std::vector<QuerySlot> slots,
                                               std::vector<Math::Vec3> vertices, std::vector<GroundedNavigationPolygon> polygons,
                                               std::vector<NavigationPolygonAdjacency> adjacency, std::vector<Math::Vec3> polygonCenters,
                                               std::vector<dtPolyRef> polygonReferences, NavigationAreaRegistry areaRegistry) noexcept
                : world_(info.world), topology_(info.topology), nearestPointHalfExtents_(info.nearestPointHalfExtents),
                  maximumResultPoints_(info.maximumResultPoints),
                  capabilities_(MakeAvailableGroundedQueryCapabilities(info.capabilityRevision,
                                                                       {.maximumNodeExpansions = info.maximumQueryNodes,
                                                                        .maximumResultPoints = info.maximumResultPoints,
                                                                        .maximumSearchDistanceMeters = info.maximumSearchDistanceMeters},
                                                                       info.maximumConcurrentQueries)),
                  mesh_(std::move(mesh)), slots_(std::move(slots)), vertices_(std::move(vertices)), polygons_(std::move(polygons)),
                  adjacency_(std::move(adjacency)), polygonCenters_(std::move(polygonCenters)),
                  polygonReferences_(std::move(polygonReferences)), areaRegistry_(std::move(areaRegistry)) {}

            [[nodiscard]] NavigationProviderCapabilities Capabilities() const noexcept override {
                return capabilities_;
            }

            [[nodiscard]] Result<NavigationPath> FindPath(const NavigationPathRequest &request,
                                                          const CancellationToken &cancellation) const override {
                if (const auto validated = ValidateRequest(request, world_, topology_, capabilities_); validated.HasError())
                    return Result<NavigationPath>::Failure(validated.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
                QueryLease lease{TryLease(slots_)};
                if (lease.Get() == nullptr)
                    return Failure<NavigationPath>(NavigationErrors::AdmissionRejected);

                QuerySlot &slot = *lease.Get();
                const auto start =
                    ResolveEndpoint(slot, request.start, nearestPointHalfExtents_, request, areaRegistry_, polygons_, polygonReferences_);
                if (start.HasError())
                    return Result<NavigationPath>::Failure(start.ErrorValue());
                const auto destination = ResolveEndpoint(slot, request.destination, nearestPointHalfExtents_, request, areaRegistry_,
                                                         polygons_, polygonReferences_);
                if (destination.HasError())
                    return Result<NavigationPath>::Failure(destination.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
                if (!start.Value().found || !destination.Value().found) {
                    NavigationPath path;
                    path.status = NavigationPathStatus::Unreachable;
                    path.stopReason = NavigationPathStopReason::DestinationUnreachable;
                    path.stopPosition = !start.Value().found ? request.start : request.destination;
                    path.stopPolygonIndex = InvalidNavigationPolygonIndex;
                    path.sourceGeneration = request.topology;
                    return Result<NavigationPath>::Success(std::move(path));
                }

                auto corridor = FindCorridor(slot, start.Value(), destination.Value(), request, areaRegistry_, polygons_, adjacency_,
                                             polygonCenters_, cancellation);
                if (corridor.HasError())
                    return Result<NavigationPath>::Failure(corridor.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
                auto path = BuildPath(slot, start.Value(), request, corridor.Value(), areaRegistry_, polygons_, polygonCenters_,
                                      polygonReferences_);
                if (path.HasError())
                    return path;
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
                return path;
            }

            /** @copydoc INavigationQueryBackend::ProjectPoint */
            [[nodiscard]] Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                                          const CancellationToken &cancellation) const override {
                return ProjectRecastDetourPoint(SpatialState(), request, cancellation);
            }

            /** @copydoc INavigationQueryBackend::SamplePosition */
            [[nodiscard]] Result<NavigationSamplePositionResult> SamplePosition(const NavigationSamplePositionRequest &request,
                                                                                const CancellationToken &cancellation) const override {
                return SampleRecastDetourPosition(SpatialState(), request, cancellation);
            }

            /** @copydoc INavigationQueryBackend::Raycast */
            [[nodiscard]] Result<NavigationRaycastResult> Raycast(const NavigationRaycastRequest &request,
                                                                  const CancellationToken &cancellation) const override {
                return RaycastRecastDetour(SpatialState(), request, cancellation);
            }

            /** @copydoc INavigationQueryBackend::QueryPolygons */
            [[nodiscard]] Result<NavigationPolygonQueryResult> QueryPolygons(const NavigationPolygonQueryRequest &request,
                                                                             const CancellationToken &cancellation) const override {
                return QueryRecastDetourPolygons(SpatialState(), request, cancellation);
            }

        private:
            [[nodiscard]] RecastDetourSpatialQueryState SpatialState() const noexcept {
                return {.world = world_,
                        .topology = topology_,
                        .nearestPointHalfExtents = nearestPointHalfExtents_,
                        .maximumResultPoints = maximumResultPoints_,
                        .capabilities = capabilities_,
                        .mesh = mesh_.get(),
                        .slots = &slots_,
                        .vertices = &vertices_,
                        .polygons = &polygons_};
            }

            NavigationWorldId world_;
            NavigationGeneration topology_;
            Math::Vec3 nearestPointHalfExtents_;
            std::uint32_t maximumResultPoints_{};
            NavigationProviderCapabilities capabilities_;
            NavMeshPtr mesh_;
            mutable std::vector<QuerySlot> slots_;
            std::vector<Math::Vec3> vertices_;
            std::vector<GroundedNavigationPolygon> polygons_;
            std::vector<NavigationPolygonAdjacency> adjacency_;
            std::vector<Math::Vec3> polygonCenters_;
            std::vector<dtPolyRef> polygonReferences_;
            NavigationAreaRegistry areaRegistry_;
        };

    }  // namespace

    /** @copydoc MakeRecastDetourNavigationQueryBackend */
    Result<std::unique_ptr<INavigationQueryBackend>> MakeRecastDetourNavigationQueryBackend(
        const RecastDetourProviderCreateInfo &info, NavMeshPtr mesh, std::vector<QuerySlot> slots, std::vector<Math::Vec3> vertices,
        std::vector<GroundedNavigationPolygon> polygons, std::vector<NavigationPolygonAdjacency> adjacency,
        std::vector<Math::Vec3> polygonCenters, std::vector<dtPolyRef> polygonReferences, NavigationAreaRegistry areaRegistry) {
        try {
            auto provider =
                std::make_unique<RecastDetourNavigationQueryBackend>(info, std::move(mesh), std::move(slots), std::move(vertices),
                                                                     std::move(polygons), std::move(adjacency), std::move(polygonCenters),
                                                                     std::move(polygonReferences), std::move(areaRegistry));
            return Result<std::unique_ptr<INavigationQueryBackend>>::Success(std::move(provider));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<INavigationQueryBackend>>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
