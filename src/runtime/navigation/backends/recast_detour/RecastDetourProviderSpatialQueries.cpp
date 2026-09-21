#include "Horo/Navigation/NavigationErrors.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        constexpr std::uint16_t TraversablePolygonFlag = 1U;
        constexpr std::size_t MaximumVerticesPerPolygon = 6;

        using Detail::Failure;
        using Detail::PointDistance;
        using Detail::ResolveHalfExtents;
        using Detail::ValidateSpatialRequirement;

        struct SpatialCandidate final {
            dtPolyRef reference{};
            std::uint32_t polygonIndex{};
            Math::Vec3 point{};
            double distance{};
        };

        struct NativeRaycastResult final {
            float hitParameter{std::numeric_limits<float>::max()};
            std::array<float, 3> hitNormal{0.0F, 1.0F, 0.0F};
            int polygonCount{};
        };

        class RecastDetourSpatialQueryBackend final {
        public:
            explicit RecastDetourSpatialQueryBackend(const RecastDetourSpatialQueryState &state)
                : world_(state.world), topology_(state.topology), nearestPointHalfExtents_(state.nearestPointHalfExtents),
                  maximumResultPoints_(state.maximumResultPoints), capabilities_(state.capabilities), mesh_(state.mesh),
                  slots_(*state.slots), vertices_(*state.vertices), polygons_(*state.polygons) {}

            /** @copydoc INavigationQueryBackend::ProjectPoint */
            [[nodiscard]] Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                                          const CancellationToken &cancellation) const {
                if (!request.world.IsValid() || request.world != world_)
                    return Failure<NavigationProjectionResult>(NavigationErrors::InvalidWorld);
                if (!request.topology.IsValid() || request.topology != topology_)
                    return Failure<NavigationProjectionResult>(NavigationErrors::StaleSnapshot);
                if (!Math::IsFinite(request.point))
                    return Failure<NavigationProjectionResult>(NavigationErrors::CapabilityDescriptorInvalid);
                if (const auto admitted = ValidateSpatialRequirement(request.requirement, NavigationQueryKind::NearestPoint, capabilities_);
                    admitted.HasError())
                    return Result<NavigationProjectionResult>::Failure(admitted.ErrorValue());
                auto extents = ResolveHalfExtents(request.halfExtents, nearestPointHalfExtents_);
                if (extents.HasError())
                    return Result<NavigationProjectionResult>::Failure(extents.ErrorValue());
                if (extents.Value().x > request.requirement.limits.maximumSearchDistanceMeters ||
                    extents.Value().y > request.requirement.limits.maximumSearchDistanceMeters ||
                    extents.Value().z > request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationProjectionResult>(NavigationErrors::QueryLimitExceeded);
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationProjectionResult>(NavigationErrors::QueryCancelled);

                QueryLease lease{TryLease(slots_)};
                if (lease.Get() == nullptr)
                    return Failure<NavigationProjectionResult>(NavigationErrors::AdmissionRejected);
                dtQueryFilter filter;
                filter.setIncludeFlags(TraversablePolygonFlag);
                auto candidate = FindNearestCandidate(*lease.Get(), request.point, extents.Value(), filter,
                                                      request.requirement.limits.maximumSearchDistanceMeters);
                if (candidate.HasError())
                    return Result<NavigationProjectionResult>::Failure(candidate.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationProjectionResult>(NavigationErrors::QueryCancelled);
                auto hit = MakeSurfaceHit(candidate.Value());
                if (hit.HasError())
                    return Result<NavigationProjectionResult>::Failure(hit.ErrorValue());
                return Result<NavigationProjectionResult>::Success({.hit = std::move(hit).Value()});
            }

            /** @copydoc INavigationQueryBackend::SamplePosition */
            [[nodiscard]] Result<NavigationSamplePositionResult> SamplePosition(const NavigationSamplePositionRequest &request,
                                                                                const CancellationToken &cancellation) const {
                if (!request.world.IsValid() || request.world != world_)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::InvalidWorld);
                if (!request.topology.IsValid() || request.topology != topology_)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::StaleSnapshot);
                if (!Math::IsFinite(request.center) || !std::isfinite(request.radiusMeters) || request.radiusMeters <= 0.0F)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::CapabilityDescriptorInvalid);
                if (const auto admitted =
                        ValidateSpatialRequirement(request.requirement, NavigationQueryKind::SamplePosition, capabilities_);
                    admitted.HasError())
                    return Result<NavigationSamplePositionResult>::Failure(admitted.ErrorValue());
                if (request.radiusMeters > request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::QueryLimitExceeded);
                if (nearestPointHalfExtents_.x > request.requirement.limits.maximumSearchDistanceMeters ||
                    nearestPointHalfExtents_.y > request.requirement.limits.maximumSearchDistanceMeters ||
                    nearestPointHalfExtents_.z > request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::QueryLimitExceeded);
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::QueryCancelled);

                QueryLease lease{TryLease(slots_)};
                if (lease.Get() == nullptr)
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::AdmissionRejected);
                dtQueryFilter filter;
                filter.setIncludeFlags(TraversablePolygonFlag);
                auto centerCandidate = FindNearestCandidate(*lease.Get(), request.center, nearestPointHalfExtents_, filter,
                                                            request.requirement.limits.maximumSearchDistanceMeters);
                if (centerCandidate.HasError())
                    return Result<NavigationSamplePositionResult>::Failure(centerCandidate.ErrorValue());
                if (centerCandidate.Value().distance > static_cast<double>(request.radiusMeters))
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::NoNavigationData);
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationSamplePositionResult>(NavigationErrors::QueryCancelled);
                auto polygonCount =
                    FindNeighbouringPolygons(*lease.Get(), centerCandidate.Value(), request.center, request.radiusMeters, filter);
                if (polygonCount.HasError())
                    return Result<NavigationSamplePositionResult>::Failure(polygonCount.ErrorValue());
                auto candidates = BuildSurfaceHits(*lease.Get(), request.center, request.radiusMeters, polygonCount.Value());
                if (candidates.HasError())
                    return Result<NavigationSamplePositionResult>::Failure(candidates.ErrorValue());
                return BuildRecastDetourSamplePositionResult(std::move(candidates).Value(), request.requirement.limits.maximumResultPoints);
            }

            /** @copydoc INavigationQueryBackend::Raycast */
            [[nodiscard]] Result<NavigationRaycastResult> Raycast(const NavigationRaycastRequest &request,
                                                                  const CancellationToken &cancellation) const {
                if (!request.world.IsValid() || request.world != world_)
                    return Failure<NavigationRaycastResult>(NavigationErrors::InvalidWorld);
                if (!request.topology.IsValid() || request.topology != topology_)
                    return Failure<NavigationRaycastResult>(NavigationErrors::StaleSnapshot);
                if (!Math::IsFinite(request.start) || !Math::IsFinite(request.destination))
                    return Failure<NavigationRaycastResult>(NavigationErrors::CapabilityDescriptorInvalid);
                if (const auto admitted = ValidateSpatialRequirement(request.requirement, NavigationQueryKind::Raycast, capabilities_);
                    admitted.HasError())
                    return Result<NavigationRaycastResult>::Failure(admitted.ErrorValue());
                auto rayLength = PointDistance(request.start, request.destination);
                if (rayLength.HasError())
                    return Result<NavigationRaycastResult>::Failure(rayLength.ErrorValue());
                if (rayLength.Value() > request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationRaycastResult>(NavigationErrors::QueryLimitExceeded);
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationRaycastResult>(NavigationErrors::QueryCancelled);

                QueryLease lease{TryLease(slots_)};
                if (lease.Get() == nullptr)
                    return Failure<NavigationRaycastResult>(NavigationErrors::AdmissionRejected);
                dtQueryFilter filter;
                filter.setIncludeFlags(TraversablePolygonFlag);
                auto startCandidate = FindNearestCandidate(*lease.Get(), request.start, nearestPointHalfExtents_, filter,
                                                           request.requirement.limits.maximumSearchDistanceMeters);
                if (startCandidate.HasError())
                    return Result<NavigationRaycastResult>::Failure(startCandidate.ErrorValue());
                auto native = RunRaycast(*lease.Get(), startCandidate.Value(), request.destination, filter);
                if (native.HasError())
                    return Result<NavigationRaycastResult>::Failure(native.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationRaycastResult>(NavigationErrors::QueryCancelled);
                return BuildRaycastResult(*lease.Get(), request, startCandidate.Value(), std::move(native).Value());
            }

            /** @copydoc INavigationQueryBackend::QueryPolygons */
            [[nodiscard]] Result<NavigationPolygonQueryResult> QueryPolygons(const NavigationPolygonQueryRequest &request,
                                                                             const CancellationToken &cancellation) const {
                if (!request.world.IsValid() || request.world != world_)
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::InvalidWorld);
                if (!request.topology.IsValid() || request.topology != topology_)
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::StaleSnapshot);
                if (!Math::IsFinite(request.center) || !Math::IsFinite(request.halfExtents) || request.halfExtents.x <= 0.0F ||
                    request.halfExtents.y <= 0.0F || request.halfExtents.z <= 0.0F)
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::CapabilityDescriptorInvalid);
                if (const auto admitted = ValidateSpatialRequirement(request.requirement, NavigationQueryKind::PolygonQuery, capabilities_);
                    admitted.HasError())
                    return Result<NavigationPolygonQueryResult>::Failure(admitted.ErrorValue());
                if (std::max({request.halfExtents.x, request.halfExtents.y, request.halfExtents.z}) >
                    request.requirement.limits.maximumSearchDistanceMeters)
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::QueryLimitExceeded);
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::QueryCancelled);

                QueryLease lease{TryLease(slots_)};
                if (lease.Get() == nullptr)
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::AdmissionRejected);
                dtQueryFilter filter;
                filter.setIncludeFlags(TraversablePolygonFlag);
                auto polygonCount = FindOverlappingPolygons(*lease.Get(), request.center, request.halfExtents, filter);
                if (polygonCount.HasError())
                    return Result<NavigationPolygonQueryResult>::Failure(polygonCount.ErrorValue());
                if (cancellation.IsCancellationRequested())
                    return Failure<NavigationPolygonQueryResult>(NavigationErrors::QueryCancelled);
                auto candidates = BuildSurfaceHits(*lease.Get(), request.center, request.requirement.limits.maximumSearchDistanceMeters,
                                                   polygonCount.Value());
                if (candidates.HasError())
                    return Result<NavigationPolygonQueryResult>::Failure(candidates.ErrorValue());
                return BuildRecastDetourPolygonQueryResult(std::move(candidates).Value(), request.requirement.limits.maximumResultPoints);
            }

        private:
            template <typename Query> [[nodiscard]] Result<int> RunPolygonQuery(QuerySlot &slot, Query &&query) const {
                int polygonCount{};
                const auto maximumPolygons = static_cast<int>(std::min<std::uint32_t>(maximumResultPoints_, slotCapacity(slot)));
                if (const dtStatus status = query(slot.polygonPath.data(), &polygonCount, maximumPolygons);
                    dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL))
                    return Failure<int>(dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ? NavigationErrors::CapacityExceeded
                                                                                    : NavigationErrors::ProviderFailed);
                if (polygonCount < 0 || polygonCount > maximumPolygons)
                    return Failure<int>(NavigationErrors::ProviderFailed);
                if (const auto validated = SortAndValidatePolygonRefs(slot, polygonCount); validated.HasError())
                    return Result<int>::Failure(validated.ErrorValue());
                return Result<int>::Success(polygonCount);
            }

            [[nodiscard]] Result<int> FindNeighbouringPolygons(QuerySlot &slot, const SpatialCandidate &centerCandidate,
                                                               const Math::Vec3 center, const float radius,
                                                               const dtQueryFilter &filter) const {
                const std::array<float, 3> centerValues{center.x, center.y, center.z};
                return RunPolygonQuery(slot, [&slot, &centerCandidate, &centerValues, radius, &filter](dtPolyRef *path, int *polygonCount,
                                                                                                       const int maximumPolygons) {
                    return slot.query->findLocalNeighbourhood(centerCandidate.reference, centerValues.data(), radius, &filter, path,
                                                              nullptr, polygonCount, maximumPolygons);
                });
            }

            [[nodiscard]] Result<std::vector<NavigationSurfaceHit>> BuildSurfaceHits(QuerySlot &slot, const Math::Vec3 center,
                                                                                     const float maximumDistance,
                                                                                     const int polygonCount) const {
                if (polygonCount < 0 || static_cast<std::size_t>(polygonCount) > slot.polygonPath.size())
                    return Failure<std::vector<NavigationSurfaceHit>>(NavigationErrors::ProviderFailed);
                try {
                    std::vector<NavigationSurfaceHit> hits;
                    hits.reserve(static_cast<std::size_t>(polygonCount));
                    const std::array<float, 3> centerValues{center.x, center.y, center.z};
                    for (int index = 0; index < polygonCount; ++index) {
                        const dtPolyRef reference = slot.polygonPath[static_cast<std::size_t>(index)];
                        const auto polygonIndex = ResolvePolygonIndex(reference);
                        if (polygonIndex.HasError())
                            return Result<std::vector<NavigationSurfaceHit>>::Failure(polygonIndex.ErrorValue());
                        std::array<float, 3> point{};
                        if (const dtStatus status = slot.query->closestPointOnPoly(reference, centerValues.data(), point.data(), nullptr);
                            dtStatusFailed(status))
                            return Failure<std::vector<NavigationSurfaceHit>>(NavigationErrors::ProviderFailed);
                        const Math::Vec3 surfacePoint{point[0], point[1], point[2]};
                        auto distance = PointDistance(center, surfacePoint);
                        if (distance.HasError())
                            return Result<std::vector<NavigationSurfaceHit>>::Failure(distance.ErrorValue());
                        if (distance.Value() > maximumDistance)
                            continue;
                        auto hit = MakeSurfaceHit({.reference = reference,
                                                   .polygonIndex = polygonIndex.Value(),
                                                   .point = surfacePoint,
                                                   .distance = distance.Value()});
                        if (hit.HasError())
                            return Result<std::vector<NavigationSurfaceHit>>::Failure(hit.ErrorValue());
                        hits.push_back(std::move(hit).Value());
                    }
                    if (hits.empty())
                        return Failure<std::vector<NavigationSurfaceHit>>(NavigationErrors::NoNavigationData);
                    SortSurfaceHits(hits);
                    return Result<std::vector<NavigationSurfaceHit>>::Success(std::move(hits));
                } catch (const std::bad_alloc &) {
                    return Failure<std::vector<NavigationSurfaceHit>>(NavigationErrors::CapacityExceeded);
                }
            }

            [[nodiscard]] Result<NativeRaycastResult> RunRaycast(QuerySlot &slot, const SpatialCandidate &startCandidate,
                                                                 const Math::Vec3 destination, const dtQueryFilter &filter) const {
                NativeRaycastResult native;
                const auto maximumPath = static_cast<int>(std::min<std::uint32_t>(maximumResultPoints_, slotCapacity(slot)));
                const std::array<float, 3> startValues{startCandidate.point.x, startCandidate.point.y, startCandidate.point.z};
                const std::array<float, 3> destinationValues{destination.x, destination.y, destination.z};
                if (const dtStatus status = slot.query->raycast(startCandidate.reference, startValues.data(), destinationValues.data(),
                                                                &filter, &native.hitParameter, native.hitNormal.data(),
                                                                slot.polygonPath.data(), &native.polygonCount, maximumPath);
                    dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL))
                    return Failure<NativeRaycastResult>(dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ? NavigationErrors::CapacityExceeded
                                                                                                    : NavigationErrors::ProviderFailed);
                if (native.polygonCount < 0 || native.polygonCount > maximumPath)
                    return Failure<NativeRaycastResult>(NavigationErrors::ProviderFailed);
                return Result<NativeRaycastResult>::Success(native);
            }

            [[nodiscard]] Result<NavigationRaycastResult> BuildRaycastResult(const QuerySlot &slot, const NavigationRaycastRequest &request,
                                                                             const SpatialCandidate &startCandidate,
                                                                             const NativeRaycastResult &native) const {
                const bool blocked = native.hitParameter != std::numeric_limits<float>::max();
                if (blocked && (!std::isfinite(native.hitParameter) || native.hitParameter < 0.0F || native.hitParameter > 1.0F))
                    return Failure<NavigationRaycastResult>(NavigationErrors::ProviderFailed);
                const Math::Vec3 rayStart = startCandidate.point;
                const Math::Vec3 hitPoint =
                    blocked ? rayStart + ((request.destination - rayStart) * native.hitParameter) : request.destination;
                const dtPolyRef terminalReference =
                    native.polygonCount > 0 ? slot.polygonPath[native.polygonCount - 1] : startCandidate.reference;
                const auto terminalPolygon = ResolvePolygonIndex(terminalReference);
                if (terminalPolygon.HasError())
                    return Result<NavigationRaycastResult>::Failure(terminalPolygon.ErrorValue());
                const Math::Vec3 nativeHitNormal{native.hitNormal[0], native.hitNormal[1], native.hitNormal[2]};
                const Math::Vec3 *normalOverride = blocked ? &nativeHitNormal : nullptr;
                auto hitDistance = PointDistance(request.start, hitPoint);
                if (hitDistance.HasError())
                    return Result<NavigationRaycastResult>::Failure(hitDistance.ErrorValue());
                auto hit = MakeSurfaceHit({.reference = terminalReference,
                                           .polygonIndex = terminalPolygon.Value(),
                                           .point = hitPoint,
                                           .distance = hitDistance.Value()},
                                          normalOverride);
                if (hit.HasError())
                    return Result<NavigationRaycastResult>::Failure(hit.ErrorValue());
                try {
                    NavigationRaycastResult result;
                    result.hit = std::move(hit).Value();
                    result.blocked = blocked;
                    result.truncated = static_cast<std::size_t>(native.polygonCount) > request.requirement.limits.maximumResultPoints;
                    const auto outputCount = std::min<std::size_t>(native.polygonCount, request.requirement.limits.maximumResultPoints);
                    result.traversedPolygons.reserve(outputCount);
                    for (std::size_t index = 0; index < outputCount; ++index) {
                        const auto polygonIndex = ResolvePolygonIndex(slot.polygonPath[index]);
                        if (polygonIndex.HasError())
                            return Result<NavigationRaycastResult>::Failure(polygonIndex.ErrorValue());
                        const auto &polygon = polygons_[polygonIndex.Value()];
                        result.traversedPolygons.push_back(
                            {.world = world_, .topology = topology_, .surface = polygon.surface, .polygonIndex = polygonIndex.Value()});
                    }
                    return Result<NavigationRaycastResult>::Success(std::move(result));
                } catch (const std::bad_alloc &) {
                    return Failure<NavigationRaycastResult>(NavigationErrors::CapacityExceeded);
                }
            }

            [[nodiscard]] std::uint32_t slotCapacity(const QuerySlot &slot) const noexcept {
                return static_cast<std::uint32_t>(slot.polygonPath.size());
            }

            [[nodiscard]] bool HasValidNativePolygon(const dtPolyRef reference) const {
                const dtMeshTile *nativeTile{};
                const dtPoly *nativePolygon{};
                return !dtStatusFailed(mesh_->getTileAndPolyByRef(reference, &nativeTile, &nativePolygon)) && nativeTile != nullptr &&
                       nativePolygon != nullptr;
            }

            [[nodiscard]] Result<std::uint32_t> ResolvePolygonIndex(const dtPolyRef reference) const {
                if (reference == 0)
                    return Failure<std::uint32_t>(NavigationErrors::ProviderFailed);
                [[maybe_unused]] unsigned int salt{};
                unsigned int tile{};
                unsigned int polygon{};
                mesh_->decodePolyId(reference, salt, tile, polygon);
                if (tile >= static_cast<unsigned int>(mesh_->getMaxTiles()) || polygon >= polygons_.size())
                    return Failure<std::uint32_t>(NavigationErrors::ProviderFailed);
                if (!HasValidNativePolygon(reference))
                    return Failure<std::uint32_t>(NavigationErrors::ProviderFailed);
                return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(polygon));
            }

            [[nodiscard]] Result<void> SortAndValidatePolygonRefs(QuerySlot &slot, const int polygonCount) const {
                if (polygonCount < 0 || static_cast<std::size_t>(polygonCount) > slot.polygonPath.size())
                    return Failure<void>(NavigationErrors::ProviderFailed);
                std::ranges::sort(slot.polygonPath.begin(), slot.polygonPath.begin() + polygonCount,
                                  [this](const dtPolyRef left, const dtPolyRef right) {
                    return std::tuple{mesh_->decodePolyIdPoly(left), left} < std::tuple{mesh_->decodePolyIdPoly(right), right};
                });
                for (int index = 0; index < polygonCount; ++index) {
                    if (const auto polygon = ResolvePolygonIndex(slot.polygonPath[static_cast<std::size_t>(index)]); polygon.HasError())
                        return Result<void>::Failure(polygon.ErrorValue());
                    if (index > 0 && mesh_->decodePolyIdPoly(slot.polygonPath[static_cast<std::size_t>(index - 1)]) ==
                                         mesh_->decodePolyIdPoly(slot.polygonPath[static_cast<std::size_t>(index)]))
                        return Failure<void>(NavigationErrors::ProviderFailed);
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<int> FindOverlappingPolygons(QuerySlot &slot, const Math::Vec3 center, const Math::Vec3 halfExtents,
                                                              const dtQueryFilter &filter) const {
                const std::array<float, 3> centerValues{center.x, center.y, center.z};
                const std::array<float, 3> extents{halfExtents.x, halfExtents.y, halfExtents.z};
                return RunPolygonQuery(slot, [&slot, &centerValues, &extents, &filter](dtPolyRef *path, int *polygonCount,
                                                                                       const int maximumPolygons) {
                    return slot.query->queryPolygons(centerValues.data(), extents.data(), &filter, path, polygonCount, maximumPolygons);
                });
            }

            [[nodiscard]] Result<SpatialCandidate> FindNearestCandidate(QuerySlot &slot, const Math::Vec3 point,
                                                                        const Math::Vec3 halfExtents, const dtQueryFilter &filter,
                                                                        const float maximumDistance) const {
                auto polygonCount = FindOverlappingPolygons(slot, point, halfExtents, filter);
                if (polygonCount.HasError())
                    return Result<SpatialCandidate>::Failure(polygonCount.ErrorValue());
                bool found{};
                SpatialCandidate best{};
                for (int index = 0; index < polygonCount.Value(); ++index) {
                    const dtPolyRef reference = slot.polygonPath[static_cast<std::size_t>(index)];
                    const auto polygonIndex = ResolvePolygonIndex(reference);
                    if (polygonIndex.HasError())
                        return Result<SpatialCandidate>::Failure(polygonIndex.ErrorValue());
                    std::array<float, 3> closest{};
                    if (const dtStatus status = slot.query->closestPointOnPoly(reference, &point.x, closest.data(), nullptr);
                        dtStatusFailed(status))
                        return Failure<SpatialCandidate>(NavigationErrors::ProviderFailed);
                    const Math::Vec3 closestPoint{closest[0], closest[1], closest[2]};
                    auto distance = PointDistance(point, closestPoint);
                    if (distance.HasError())
                        return Result<SpatialCandidate>::Failure(distance.ErrorValue());
                    const SpatialCandidate candidate{.reference = reference,
                                                     .polygonIndex = polygonIndex.Value(),
                                                     .point = closestPoint,
                                                     .distance = distance.Value()};
                    if (!found || std::tie(candidate.distance, candidate.polygonIndex) < std::tie(best.distance, best.polygonIndex)) {
                        best = candidate;
                        found = true;
                    }
                }
                if (!found || best.distance > maximumDistance)
                    return Failure<SpatialCandidate>(NavigationErrors::NoNavigationData);
                return Result<SpatialCandidate>::Success(best);
            }

            [[nodiscard]] Result<Math::Vec3> PolygonNormal(const std::uint32_t polygonIndex) const {
                if (polygonIndex >= polygons_.size())
                    return Result<Math::Vec3>::Failure(MakeError(NavigationErrors::ProviderFailed));
                const GroundedNavigationPolygon &polygon = polygons_[polygonIndex];
                if (polygon.vertexCount < 3 || polygon.vertexCount > MaximumVerticesPerPolygon ||
                    std::ranges::any_of(std::span{polygon.vertexIndices}.first(polygon.vertexCount), [this](const std::uint32_t index) {
                    return index >= vertices_.size();
                }))
                    return Result<Math::Vec3>::Failure(MakeError(NavigationErrors::ProviderFailed));
                const Math::Vec3 &first = vertices_[polygon.vertexIndices[0]];
                const Math::Vec3 &second = vertices_[polygon.vertexIndices[1]];
                const Math::Vec3 &third = vertices_[polygon.vertexIndices[2]];
                const double firstX = static_cast<double>(second.x) - first.x;
                const double firstY = static_cast<double>(second.y) - first.y;
                const double firstZ = static_cast<double>(second.z) - first.z;
                const double secondX = static_cast<double>(third.x) - first.x;
                const double secondY = static_cast<double>(third.y) - first.y;
                const double secondZ = static_cast<double>(third.z) - first.z;
                double normalX = firstY * secondZ - firstZ * secondY;
                double normalY = firstZ * secondX - firstX * secondZ;
                double normalZ = firstX * secondY - firstY * secondX;
                const double length = std::hypot(normalX, normalY, normalZ);
                if (!std::isfinite(length) || length <= std::numeric_limits<double>::epsilon())
                    return Result<Math::Vec3>::Failure(MakeError(NavigationErrors::ProviderFailed));
                if (normalY < 0.0) {
                    normalX = -normalX;
                    normalY = -normalY;
                    normalZ = -normalZ;
                }
                return Result<Math::Vec3>::Success(
                    {static_cast<float>(normalX / length), static_cast<float>(normalY / length), static_cast<float>(normalZ / length)});
            }

            [[nodiscard]] Result<NavigationSurfaceHit> MakeSurfaceHit(const SpatialCandidate &candidate,
                                                                      const Math::Vec3 *normalOverride = nullptr) const {
                if (!Math::IsFinite(candidate.point) || !std::isfinite(candidate.distance) || candidate.distance < 0.0 ||
                    candidate.distance > std::numeric_limits<float>::max())
                    return Failure<NavigationSurfaceHit>(NavigationErrors::ProviderFailed);
                auto normal = PolygonNormal(candidate.polygonIndex);
                if (normal.HasError())
                    return Result<NavigationSurfaceHit>::Failure(normal.ErrorValue());
                if (normalOverride != nullptr && Math::IsFinite(*normalOverride)) {
                    const double length = std::hypot(static_cast<double>(normalOverride->x), static_cast<double>(normalOverride->y),
                                                     static_cast<double>(normalOverride->z));
                    if (std::isfinite(length) && length > std::numeric_limits<double>::epsilon())
                        normal = Result<Math::Vec3>::Success({static_cast<float>(normalOverride->x / length),
                                                              static_cast<float>(normalOverride->y / length),
                                                              static_cast<float>(normalOverride->z / length)});
                }
                const GroundedNavigationPolygon &polygon = polygons_[candidate.polygonIndex];
                return Result<NavigationSurfaceHit>::Success({.position = candidate.point,
                                                              .normal = normal.Value(),
                                                              .distanceMeters = static_cast<float>(candidate.distance),
                                                              .surface = polygon.surface,
                                                              .area = polygon.area,
                                                              .provenance = {.world = world_,
                                                                             .topology = topology_,
                                                                             .surface = polygon.surface,
                                                                             .polygonIndex = candidate.polygonIndex}});
            }

            static void SortSurfaceHits(std::vector<NavigationSurfaceHit> &hits) {
                std::ranges::sort(hits, [](const NavigationSurfaceHit &left, const NavigationSurfaceHit &right) {
                    return std::tie(left.distanceMeters, left.provenance.polygonIndex) <
                           std::tie(right.distanceMeters, right.provenance.polygonIndex);
                });
            }

            NavigationWorldId world_;
            NavigationGeneration topology_;
            Math::Vec3 nearestPointHalfExtents_;
            std::uint32_t maximumResultPoints_{};
            NavigationProviderCapabilities capabilities_;
            dtNavMesh *mesh_{};
            std::vector<QuerySlot> &slots_;
            const std::vector<Math::Vec3> &vertices_;
            const std::vector<GroundedNavigationPolygon> &polygons_;
        };

    }  // namespace

    Result<NavigationProjectionResult> ProjectRecastDetourPoint(const RecastDetourSpatialQueryState &state,
                                                                const NavigationPointProjectionRequest &request,
                                                                const CancellationToken &cancellation) {
        return RecastDetourSpatialQueryBackend{state}.ProjectPoint(request, cancellation);
    }

    Result<NavigationSamplePositionResult> SampleRecastDetourPosition(const RecastDetourSpatialQueryState &state,
                                                                      const NavigationSamplePositionRequest &request,
                                                                      const CancellationToken &cancellation) {
        return RecastDetourSpatialQueryBackend{state}.SamplePosition(request, cancellation);
    }

    Result<NavigationRaycastResult> RaycastRecastDetour(const RecastDetourSpatialQueryState &state, const NavigationRaycastRequest &request,
                                                        const CancellationToken &cancellation) {
        return RecastDetourSpatialQueryBackend{state}.Raycast(request, cancellation);
    }

    Result<NavigationPolygonQueryResult> QueryRecastDetourPolygons(const RecastDetourSpatialQueryState &state,
                                                                   const NavigationPolygonQueryRequest &request,
                                                                   const CancellationToken &cancellation) {
        return RecastDetourSpatialQueryBackend{state}.QueryPolygons(request, cancellation);
    }
}  // namespace Horo::Navigation
