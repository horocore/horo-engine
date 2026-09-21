#include "Horo/Navigation/NavigationErrors.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderQueriesInternal.h"

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
            if (!std::isfinite(request.clearanceMeters) || request.clearanceMeters < 0.0F)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (request.requirement.limits.maximumResultPoints < 2U)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (const auto admitted = AdmitNavigationQuery(capabilities, capabilities.revision, request.requirement); admitted.HasError())
                return admitted;
            const NavigationQueryLimits &available = capabilities.queryLimits[static_cast<std::size_t>(request.requirement.query)]
                                                                             [static_cast<std::size_t>(request.requirement.quality)];
            if ((request.outputLimits.maximumCorridorPolygons != 0 &&
                 request.outputLimits.maximumCorridorPolygons > available.maximumNodeExpansions) ||
                (request.outputLimits.maximumPortals != 0 && request.outputLimits.maximumPortals > available.maximumResultPoints) ||
                (request.outputLimits.maximumWaypoints != 0 && request.outputLimits.maximumWaypoints > available.maximumResultPoints) ||
                (request.outputLimits.maximumWaypoints != 0 && request.outputLimits.maximumWaypoints < 2U))
                return Failure<void>(NavigationErrors::QueryLimitExceeded);
            if (const double distance = std::hypot(static_cast<double>(request.destination.x) - request.start.x,
                                                   static_cast<double>(request.destination.y) - request.start.y,
                                                   static_cast<double>(request.destination.z) - request.start.z);
                !std::isfinite(distance) || distance > request.requirement.limits.maximumSearchDistanceMeters)
                return Failure<void>(NavigationErrors::QueryLimitExceeded);
            return Result<void>::Success();
        }

        using RecastDetourQueries::BuildPath;
        using RecastDetourQueries::CorridorSearch;
        using RecastDetourQueries::PathBuildContext;
        using RecastDetourQueries::QueryEndpoint;

        struct CorridorSearchContext final {
            QuerySlot &slot;
            const QueryEndpoint &start;
            const QueryEndpoint &destination;
            const NavigationPathRequest &request;
            const NavigationAreaRegistry &areaRegistry;
            const std::vector<GroundedNavigationPolygon> &polygons;
            const std::vector<NavigationPolygonAdjacency> &adjacency;
            const std::vector<Math::Vec3> &centers;
            const CancellationToken &cancellation;
        };

        struct CorridorSearchState final {
            std::uint32_t nodeLimit{};
            std::uint32_t nodeCount{};
            std::uint32_t expansions{};
            std::uint32_t bestNode{InvalidNavigationPolygonIndex};
            NavigationPathStatus terminalStatus{NavigationPathStatus::Unreachable};
            NavigationPathStopReason terminalReason{NavigationPathStopReason::DestinationUnreachable};
            bool nodeBudgetExhausted{};
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

        [[nodiscard]] Result<float> InitializeCorridorSearch(CorridorSearchContext &context, CorridorSearchState &state) {
            context.slot.openNodes.clear();
            state.nodeLimit = std::min({context.request.requirement.limits.maximumNodeExpansions,
                                        static_cast<std::uint32_t>(context.slot.searchNodes.size()),
                                        static_cast<std::uint32_t>(context.slot.openNodes.capacity())});
            if (state.nodeLimit == 0)
                return Failure<float>(NavigationErrors::CapacityExceeded);
            for (std::uint32_t index = 0; index < state.nodeLimit; ++index)
                context.slot.searchNodes[index] = {};

            const auto minimumCost = MinimumTraversalCost(context.areaRegistry, context.request.filter, context.polygons);
            if (minimumCost.HasError())
                return Result<float>::Failure(minimumCost.ErrorValue());
            const auto startHeuristic =
                Heuristic(context.centers[context.start.polygon], context.centers[context.destination.polygon], minimumCost.Value());
            if (startHeuristic.HasError())
                return Result<float>::Failure(startHeuristic.ErrorValue());
            context.slot.searchNodes[0] = {.polygon = context.start.polygon,
                                           .parent = InvalidNavigationPolygonIndex,
                                           .cost = 0.0F,
                                           .estimatedTotalCost = startHeuristic.Value(),
                                           .open = true,
                                           .closed = false};
            context.slot.openNodes.push_back(0);
            state.nodeCount = 1;
            return minimumCost;
        }

        [[nodiscard]] Result<float> CandidateSearchCost(const Math::Vec3 currentCenter, const Math::Vec3 neighborCenter,
                                                        const float currentCost, const float traversalCost) {
            const auto edgeDistance = Detail::PointDistance(currentCenter, neighborCenter);
            if (edgeDistance.HasError())
                return edgeDistance;
            const double candidateCost = static_cast<double>(currentCost) + static_cast<double>(edgeDistance.Value()) * traversalCost;
            if (!std::isfinite(candidateCost) || candidateCost > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::QueryLimitExceeded);
            return Result<float>::Success(static_cast<float>(candidateCost));
        }

        [[nodiscard]] Result<float> EstimatedSearchCost(const CorridorSearchContext &context, const std::uint32_t polygon,
                                                        const float candidateCost, const float minimumCost) {
            const auto estimate = Heuristic(context.centers[polygon], context.centers[context.destination.polygon], minimumCost);
            if (estimate.HasError())
                return Result<float>::Failure(estimate.ErrorValue());
            const double estimatedTotalCost = static_cast<double>(candidateCost) + static_cast<double>(estimate.Value());
            if (!std::isfinite(estimatedTotalCost) || estimatedTotalCost > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::QueryLimitExceeded);
            return Result<float>::Success(static_cast<float>(estimatedTotalCost));
        }

        [[nodiscard]] Result<void> AddNeighborNode(CorridorSearchContext &context, CorridorSearchState &state,
                                                   const std::uint32_t currentIndex, const std::uint32_t neighbor,
                                                   const float candidateCost, const float minimumCost) {
            if (state.nodeCount >= state.nodeLimit) {
                state.nodeBudgetExhausted = true;
                return Result<void>::Success();
            }
            const std::uint32_t neighborNode = state.nodeCount++;
            const auto estimatedCost = EstimatedSearchCost(context, neighbor, candidateCost, minimumCost);
            if (estimatedCost.HasError())
                return Result<void>::Failure(estimatedCost.ErrorValue());
            context.slot.searchNodes[neighborNode] = {.polygon = neighbor,
                                                      .parent = currentIndex,
                                                      .cost = candidateCost,
                                                      .estimatedTotalCost = estimatedCost.Value(),
                                                      .open = true,
                                                      .closed = false};
            context.slot.openNodes.push_back(neighborNode);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ConsiderNeighbor(CorridorSearchContext &context, CorridorSearchState &state,
                                                    const std::uint32_t currentIndex, const std::uint32_t neighbor,
                                                    const float minimumCost) {
            if (context.cancellation.IsCancellationRequested())
                return Failure<void>(NavigationErrors::QueryCancelled);
            const auto traversal = context.areaRegistry.ResolveTraversal(context.request.filter, context.polygons[neighbor].area);
            if (traversal.HasError())
                return Result<void>::Failure(traversal.ErrorValue());
            if (!traversal.Value().traversable)
                return Result<void>::Success();
            const auto candidateCost =
                CandidateSearchCost(context.centers[context.slot.searchNodes[currentIndex].polygon], context.centers[neighbor],
                                    context.slot.searchNodes[currentIndex].cost, traversal.Value().traversalCost);
            if (candidateCost.HasError())
                return Result<void>::Failure(candidateCost.ErrorValue());

            std::uint32_t neighborNode = FindSearchNode(context.slot, state.nodeCount, neighbor);
            if (neighborNode == InvalidNavigationPolygonIndex)
                return AddNeighborNode(context, state, currentIndex, neighbor, candidateCost.Value(), minimumCost);

            NavigationAStarNode &existing = context.slot.searchNodes[neighborNode];
            const float candidate = candidateCost.Value();
            if (existing.closed && candidate >= existing.cost)
                return Result<void>::Success();
            if (candidate > existing.cost ||
                (candidate == existing.cost && existing.parent != InvalidNavigationPolygonIndex &&
                 context.slot.searchNodes[existing.parent].polygon <= context.slot.searchNodes[currentIndex].polygon))
                return Result<void>::Success();
            const auto estimatedCost = EstimatedSearchCost(context, neighbor, candidate, minimumCost);
            if (estimatedCost.HasError())
                return Result<void>::Failure(estimatedCost.ErrorValue());
            existing.parent = currentIndex;
            existing.cost = candidate;
            existing.estimatedTotalCost = estimatedCost.Value();
            existing.open = true;
            existing.closed = false;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ExpandCorridorNode(CorridorSearchContext &context, CorridorSearchState &state,
                                                      const std::uint32_t currentIndex, const float minimumCost) {
            NavigationAStarNode &current = context.slot.searchNodes[currentIndex];
            current.open = false;
            current.closed = true;
            ++state.expansions;
            const NavigationPolygonAdjacency &neighbors = context.adjacency[current.polygon];
            for (std::uint8_t neighborIndex = 0; neighborIndex < neighbors.count; ++neighborIndex) {
                const auto considered = ConsiderNeighbor(context, state, currentIndex, neighbors.neighbors[neighborIndex], minimumCost);
                if (considered.HasError())
                    return considered;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> RunCorridorSearch(CorridorSearchContext &context, CorridorSearchState &state, const float minimumCost) {
            while (true) {
                if (context.cancellation.IsCancellationRequested())
                    return Failure<void>(NavigationErrors::QueryCancelled);
                const std::uint32_t currentIndex = SelectBestOpenNode(context.slot);
                if (currentIndex == InvalidNavigationPolygonIndex)
                    return Result<void>::Success();
                NavigationAStarNode &current = context.slot.searchNodes[currentIndex];
                if (current.polygon == context.destination.polygon) {
                    state.bestNode = currentIndex;
                    state.terminalStatus = NavigationPathStatus::Reachable;
                    state.terminalReason = NavigationPathStopReason::None;
                    return Result<void>::Success();
                }
                if (state.expansions >= context.request.requirement.limits.maximumNodeExpansions) {
                    state.bestNode = currentIndex;
                    state.terminalStatus = NavigationPathStatus::BudgetExceeded;
                    state.terminalReason = NavigationPathStopReason::NodeBudgetExceeded;
                    return Result<void>::Success();
                }
                state.bestNode = currentIndex;
                const auto expanded = ExpandCorridorNode(context, state, currentIndex, minimumCost);
                if (expanded.HasError())
                    return expanded;
            }
        }

        [[nodiscard]] Result<CorridorSearch> FinalizeCorridorSearch(CorridorSearchContext &context, CorridorSearchState &state) {
            CorridorSearch result{.status = NavigationPathStatus::Unreachable,
                                  .stopReason = NavigationPathStopReason::DestinationUnreachable,
                                  .terminalNode = state.bestNode};
            if (state.terminalStatus == NavigationPathStatus::Reachable || state.terminalStatus == NavigationPathStatus::BudgetExceeded) {
                result.status = state.terminalStatus;
                result.stopReason = state.terminalReason;
            } else if (state.nodeBudgetExhausted) {
                result.status = NavigationPathStatus::BudgetExceeded;
                result.stopReason = NavigationPathStopReason::NodeBudgetExceeded;
            }
            if (result.status != NavigationPathStatus::Reachable)
                state.bestNode = SelectBestKnownNode(context.slot, state.nodeCount);
            result.terminalNode = state.bestNode;
            std::uint32_t cursor = result.terminalNode;
            while (cursor != InvalidNavigationPolygonIndex && result.polygonCount < context.slot.polygonPathIndices.size()) {
                context.slot.polygonPathIndices[result.polygonCount++] = context.slot.searchNodes[cursor].polygon;
                cursor = context.slot.searchNodes[cursor].parent;
            }
            std::ranges::reverse(std::span{context.slot.polygonPathIndices.data(), result.polygonCount});
            return Result<CorridorSearch>::Success(result);
        }

        [[nodiscard]] Result<CorridorSearch> FindCorridor(CorridorSearchContext &context) {
            CorridorSearchState state;
            const auto minimumCost = InitializeCorridorSearch(context, state);
            if (minimumCost.HasError())
                return Result<CorridorSearch>::Failure(minimumCost.ErrorValue());
            const auto searched = RunCorridorSearch(context, state, minimumCost.Value());
            if (searched.HasError())
                return Result<CorridorSearch>::Failure(searched.ErrorValue());
            return FinalizeCorridorSearch(context, state);
        }

        [[nodiscard]] NavigationPath MakeUnreachablePath(const NavigationPathRequest &request, const bool startFound) {
            NavigationPath path;
            path.status = NavigationPathStatus::Unreachable;
            path.stopReason = NavigationPathStopReason::DestinationUnreachable;
            path.stopPosition = startFound ? request.destination : request.start;
            path.stopPolygonIndex = InvalidNavigationPolygonIndex;
            path.sourceGeneration = request.topology;
            return path;
        }

        struct PathQueryContext final {
            QuerySlot &slot;
            const QueryEndpoint &start;
            const QueryEndpoint &destination;
            const NavigationPathRequest &request;
            const NavigationAreaRegistry &areaRegistry;
            const std::vector<GroundedNavigationPolygon> &polygons;
            const std::vector<NavigationPolygonAdjacency> &adjacency;
            const std::vector<Math::Vec3> &vertices;
            const std::vector<Math::Vec3> &centers;
            const std::vector<dtPolyRef> &references;
            float defaultClearanceMeters{};
        };

        [[nodiscard]] Result<NavigationPath> ExecutePathQuery(PathQueryContext &context, const CancellationToken &cancellation) {
            CorridorSearchContext corridorContext{.slot = context.slot,
                                                  .start = context.start,
                                                  .destination = context.destination,
                                                  .request = context.request,
                                                  .areaRegistry = context.areaRegistry,
                                                  .polygons = context.polygons,
                                                  .adjacency = context.adjacency,
                                                  .centers = context.centers,
                                                  .cancellation = cancellation};
            auto corridor = FindCorridor(corridorContext);
            if (corridor.HasError())
                return Result<NavigationPath>::Failure(corridor.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
            PathBuildContext pathContext{.slot = context.slot,
                                         .start = context.start,
                                         .request = context.request,
                                         .search = corridor.Value(),
                                         .areaRegistry = context.areaRegistry,
                                         .polygons = context.polygons,
                                         .vertices = context.vertices,
                                         .centers = context.centers,
                                         .references = context.references,
                                         .defaultClearanceMeters = context.defaultClearanceMeters};
            auto path = BuildPath(pathContext);
            if (path.HasError())
                return path;
            if (cancellation.IsCancellationRequested())
                return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
            return path;
        }

        class RecastDetourNavigationQueryBackend final : public INavigationQueryBackend {
        public:
            RecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &info, RecastDetourQueryBackendData data) noexcept
                : world_(info.world), topology_(info.topology), nearestPointHalfExtents_(info.nearestPointHalfExtents),
                  defaultClearanceMeters_(info.walkableRadiusMeters), maximumResultPoints_(info.maximumResultPoints),
                  capabilities_(MakeAvailableGroundedQueryCapabilities(info.capabilityRevision,
                                                                       {.maximumNodeExpansions = info.maximumQueryNodes,
                                                                        .maximumResultPoints = info.maximumResultPoints,
                                                                        .maximumSearchDistanceMeters = info.maximumSearchDistanceMeters},
                                                                       info.maximumConcurrentQueries)),
                  mesh_(std::move(data.mesh)), slots_(std::move(data.slots)), vertices_(std::move(data.vertices)),
                  polygons_(std::move(data.polygons)), adjacency_(std::move(data.adjacency)),
                  polygonCenters_(std::move(data.polygonCenters)), polygonReferences_(std::move(data.polygonReferences)),
                  areaRegistry_(std::move(data.areaRegistry)) {}

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
                    return Result<NavigationPath>::Success(MakeUnreachablePath(request, start.Value().found));
                }

                PathQueryContext context{.slot = slot,
                                         .start = start.Value(),
                                         .destination = destination.Value(),
                                         .request = request,
                                         .areaRegistry = areaRegistry_,
                                         .polygons = polygons_,
                                         .adjacency = adjacency_,
                                         .vertices = vertices_,
                                         .centers = polygonCenters_,
                                         .references = polygonReferences_,
                                         .defaultClearanceMeters = defaultClearanceMeters_};
                return ExecutePathQuery(context, cancellation);
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
            float defaultClearanceMeters_{};
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
    Result<std::unique_ptr<INavigationQueryBackend>> MakeRecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &info,
                                                                                            RecastDetourQueryBackendData data) {
        try {
            auto provider = std::make_unique<RecastDetourNavigationQueryBackend>(info, std::move(data));
            return Result<std::unique_ptr<INavigationQueryBackend>>::Success(std::move(provider));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<INavigationQueryBackend>>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
