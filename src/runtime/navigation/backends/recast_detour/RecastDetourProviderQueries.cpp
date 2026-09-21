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
        constexpr std::uint16_t TraversablePolygonFlag = 1U;
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
            if (!Math::IsFinite(request.start) || !Math::IsFinite(request.destination))
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

        struct QueryEndpoints final {
            dtPolyRef startPolygon{};
            dtPolyRef destinationPolygon{};
            std::array<float, 3> start{};
            std::array<float, 3> destination{};
        };

        [[nodiscard]] Result<QueryEndpoints> ResolveEndpoints(const QuerySlot &slot, const NavigationPathRequest &request,
                                                              const Math::Vec3 halfExtents, const dtQueryFilter &filter) {
            const std::array<float, 3> start{request.start.x, request.start.y, request.start.z};
            const std::array<float, 3> destination{request.destination.x, request.destination.y, request.destination.z};
            const std::array<float, 3> extents{halfExtents.x, halfExtents.y, halfExtents.z};
            QueryEndpoints endpoints;
            if (const dtStatus status =
                    slot.query->findNearestPoly(start.data(), extents.data(), &filter, &endpoints.startPolygon, endpoints.start.data());
                dtStatusFailed(status))
                return Failure<QueryEndpoints>(NavigationErrors::ProviderFailed);
            if (const dtStatus status = slot.query->findNearestPoly(destination.data(), extents.data(), &filter,
                                                                    &endpoints.destinationPolygon, endpoints.destination.data());
                dtStatusFailed(status))
                return Failure<QueryEndpoints>(NavigationErrors::ProviderFailed);
            if (endpoints.startPolygon == 0 || endpoints.destinationPolygon == 0)
                return Failure<QueryEndpoints>(NavigationErrors::NoNavigationData);
            return Result<QueryEndpoints>::Success(endpoints);
        }

        [[nodiscard]] Result<int> FindCorridor(QuerySlot &slot, const QueryEndpoints &endpoints, const dtQueryFilter &filter,
                                               const std::uint32_t maximumNodes) {
            int polygonCount{};
            const auto scratchCapacity = static_cast<std::uint32_t>(slot.polygonPath.size());
            const auto boundedNodes = static_cast<int>(std::min(maximumNodes, scratchCapacity));
            if (const dtStatus status =
                    slot.query->findPath(endpoints.startPolygon, endpoints.destinationPolygon, endpoints.start.data(),
                                         endpoints.destination.data(), &filter, slot.polygonPath.data(), &polygonCount, boundedNodes);
                dtStatusFailed(status))
                return Failure<int>(dtStatusDetail(status, DT_OUT_OF_NODES) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL)
                                        ? NavigationErrors::CapacityExceeded
                                        : NavigationErrors::ProviderFailed);
            if (polygonCount == 0 || slot.polygonPath[polygonCount - 1] != endpoints.destinationPolygon)
                return Failure<int>(NavigationErrors::NoNavigationData);
            return Result<int>::Success(polygonCount);
        }

        [[nodiscard]] Result<NavigationPath> BuildPath(QuerySlot &slot, const QueryEndpoints &endpoints,
                                                       const NavigationPathRequest &request, const int polygonCount) {
            int pointCount{};
            const auto scratchCapacity = static_cast<std::uint32_t>(slot.straightPoints.size() / 3U);
            const auto boundedPoints = static_cast<int>(std::min(request.requirement.limits.maximumResultPoints, scratchCapacity));
            if (const dtStatus status =
                    slot.query->findStraightPath(endpoints.start.data(), endpoints.destination.data(), slot.polygonPath.data(),
                                                 polygonCount, slot.straightPoints.data(), slot.straightFlags.data(),
                                                 slot.straightPolygons.data(), &pointCount, boundedPoints);
                dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL))
                return Failure<NavigationPath>(dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ? NavigationErrors::CapacityExceeded
                                                                                           : NavigationErrors::ProviderFailed);
            try {
                NavigationPath path;
                path.points.reserve(static_cast<std::size_t>(pointCount));
                for (int index = 0; index < pointCount; ++index) {
                    const std::size_t offset = static_cast<std::size_t>(index) * 3U;
                    path.points.push_back(
                        {slot.straightPoints[offset], slot.straightPoints[offset + 1U], slot.straightPoints[offset + 2U]});
                }
                if (path.points.empty())
                    return Failure<NavigationPath>(NavigationErrors::NoNavigationData);
                path.points.front() = request.start;
                path.points.back() = request.destination;
                auto length = PathLength(path.points);
                if (length.HasError())
                    return Result<NavigationPath>::Failure(length.ErrorValue());
                path.lengthMeters = length.Value();
                return Result<NavigationPath>::Success(std::move(path));
            } catch (const std::bad_alloc &) {
                return Failure<NavigationPath>(NavigationErrors::CapacityExceeded);
            }
        }

        class RecastDetourNavigationQueryBackend final : public INavigationQueryBackend {
        public:
            RecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &info, NavMeshPtr mesh, std::vector<QuerySlot> slots,
                                               std::vector<Math::Vec3> vertices, std::vector<GroundedNavigationPolygon> polygons) noexcept
                : world_(info.world), topology_(info.topology), nearestPointHalfExtents_(info.nearestPointHalfExtents),
                  maximumResultPoints_(info.maximumResultPoints),
                  capabilities_(MakeAvailableGroundedQueryCapabilities(info.capabilityRevision,
                                                                       {.maximumNodeExpansions = info.maximumQueryNodes,
                                                                        .maximumResultPoints = info.maximumResultPoints,
                                                                        .maximumSearchDistanceMeters = info.maximumSearchDistanceMeters},
                                                                       info.maximumConcurrentQueries)),
                  mesh_(std::move(mesh)), slots_(std::move(slots)), vertices_(std::move(vertices)), polygons_(std::move(polygons)) {}

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
                dtQueryFilter filter;
                filter.setIncludeFlags(TraversablePolygonFlag);
                auto endpoints = ResolveEndpoints(slot, request, nearestPointHalfExtents_, filter);
                if (endpoints.HasError())
                    return Result<NavigationPath>::Failure(endpoints.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);

                auto corridor = FindCorridor(slot, endpoints.Value(), filter, request.requirement.limits.maximumNodeExpansions);
                if (corridor.HasError())
                    return Result<NavigationPath>::Failure(corridor.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPath>(NavigationErrors::QueryCancelled);
                auto path = BuildPath(slot, endpoints.Value(), request, corridor.Value());
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
        };

    }  // namespace

    /** @copydoc MakeRecastDetourNavigationQueryBackend */
    Result<std::unique_ptr<INavigationQueryBackend>> MakeRecastDetourNavigationQueryBackend(
        const RecastDetourProviderCreateInfo &info, NavMeshPtr mesh, std::vector<QuerySlot> slots, std::vector<Math::Vec3> vertices,
        std::vector<GroundedNavigationPolygon> polygons) {
        try {
            auto provider = std::make_unique<RecastDetourNavigationQueryBackend>(info, std::move(mesh), std::move(slots),
                                                                                 std::move(vertices), std::move(polygons));
            return Result<std::unique_ptr<INavigationQueryBackend>>::Success(std::move(provider));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<INavigationQueryBackend>>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
