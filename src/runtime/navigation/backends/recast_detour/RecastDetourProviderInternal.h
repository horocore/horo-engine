#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    struct NavMeshDeleter final {
        void operator()(dtNavMesh *mesh) const noexcept {
            dtFreeNavMesh(mesh);
        }
    };

    struct QueryDeleter final {
        void operator()(dtNavMeshQuery *query) const noexcept {
            dtFreeNavMeshQuery(query);
        }
    };

    using NavMeshPtr = std::unique_ptr<dtNavMesh, NavMeshDeleter>;
    using QueryPtr = std::unique_ptr<dtNavMeshQuery, QueryDeleter>;

    struct QuerySlot final {
        QuerySlot() = default;

        ~QuerySlot() {
            leased.store(false);
        }

        QuerySlot(const QuerySlot &) = delete;
        QuerySlot &operator=(const QuerySlot &) = delete;

        QuerySlot(QuerySlot &&other) noexcept
            : query(std::move(other.query)), polygonPath(std::move(other.polygonPath)), straightPoints(std::move(other.straightPoints)),
              straightFlags(std::move(other.straightFlags)), straightPolygons(std::move(other.straightPolygons)),
              leased(other.leased.load()) {
            other.leased.store(false);
        }

        QuerySlot &operator=(QuerySlot &&) = delete;

        QueryPtr query;
        std::vector<dtPolyRef> polygonPath;
        std::vector<float> straightPoints;
        std::vector<unsigned char> straightFlags;
        std::vector<dtPolyRef> straightPolygons;
        std::atomic<bool> leased{false};
    };

    class QueryLease final {
    public:
        explicit QueryLease(QuerySlot *slot) noexcept : slot_(slot) {}

        QueryLease(const QueryLease &) = delete;
        QueryLease &operator=(const QueryLease &) = delete;
        QueryLease(QueryLease &&) = delete;
        QueryLease &operator=(QueryLease &&) = delete;

        ~QueryLease() {
            if (slot_ != nullptr)
                slot_->leased.store(false);
        }

        [[nodiscard]] QuerySlot *Get() const noexcept {
            return slot_;
        }

    private:
        QuerySlot *slot_{};
    };

    [[nodiscard]] inline QuerySlot *TryLease(std::vector<QuerySlot> &slots) noexcept {
        for (QuerySlot &slot : slots) {
            bool expected = false;
            if (slot.leased.compare_exchange_strong(expected, true))
                return &slot;
        }
        return nullptr;
    }

    namespace Detail {
        template <typename T> [[nodiscard]] inline Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] inline Result<float> PointDistance(const Math::Vec3 first, const Math::Vec3 second) {
            if (!Math::IsFinite(first) || !Math::IsFinite(second))
                return Failure<float>(NavigationErrors::ProviderFailed);
            const double distance = std::hypot(static_cast<double>(second.x) - first.x, static_cast<double>(second.y) - first.y,
                                               static_cast<double>(second.z) - first.z);
            if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max())
                return Failure<float>(NavigationErrors::QueryLimitExceeded);
            return Result<float>::Success(static_cast<float>(distance));
        }

        [[nodiscard]] inline Result<Math::Vec3> ResolveHalfExtents(const Math::Vec3 requested, const Math::Vec3 configured) {
            const bool useConfigured = requested.x == 0.0F && requested.y == 0.0F && requested.z == 0.0F;
            const Math::Vec3 extents = useConfigured ? configured : requested;
            if (!Math::IsFinite(extents) || extents.x <= 0.0F || extents.y <= 0.0F || extents.z <= 0.0F)
                return Failure<Math::Vec3>(NavigationErrors::CapabilityDescriptorInvalid);
            return Result<Math::Vec3>::Success(extents);
        }

        [[nodiscard]] inline Result<void> ValidateSpatialRequirement(const NavigationQueryRequirement &requirement,
                                                                     const NavigationQueryKind expected,
                                                                     const NavigationProviderCapabilities &capabilities) {
            if (requirement.query != expected)
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            return AdmitNavigationQuery(capabilities, capabilities.revision, requirement);
        }
    }  // namespace Detail

    struct RecastDetourSpatialQueryState final {
        NavigationWorldId world;
        NavigationGeneration topology;
        Math::Vec3 nearestPointHalfExtents;
        std::uint32_t maximumResultPoints{};
        NavigationProviderCapabilities capabilities;
        dtNavMesh *mesh{};
        std::vector<QuerySlot> *slots{};
        const std::vector<Math::Vec3> *vertices{};
        const std::vector<GroundedNavigationPolygon> *polygons{};
    };

    [[nodiscard]] Result<NavigationProjectionResult> ProjectRecastDetourPoint(const RecastDetourSpatialQueryState &state,
                                                                              const NavigationPointProjectionRequest &request,
                                                                              const CancellationToken &cancellation);
    [[nodiscard]] Result<NavigationSamplePositionResult> SampleRecastDetourPosition(const RecastDetourSpatialQueryState &state,
                                                                                    const NavigationSamplePositionRequest &request,
                                                                                    const CancellationToken &cancellation);
    [[nodiscard]] Result<NavigationRaycastResult> RaycastRecastDetour(const RecastDetourSpatialQueryState &state,
                                                                      const NavigationRaycastRequest &request,
                                                                      const CancellationToken &cancellation);
    [[nodiscard]] Result<NavigationPolygonQueryResult> QueryRecastDetourPolygons(const RecastDetourSpatialQueryState &state,
                                                                                 const NavigationPolygonQueryRequest &request,
                                                                                 const CancellationToken &cancellation);
    [[nodiscard]] Result<NavigationSamplePositionResult> BuildRecastDetourSamplePositionResult(std::vector<NavigationSurfaceHit> hits,
                                                                                               std::uint32_t maximumResultPoints);
    [[nodiscard]] Result<NavigationPolygonQueryResult> BuildRecastDetourPolygonQueryResult(std::vector<NavigationSurfaceHit> hits,
                                                                                           std::uint32_t maximumResultPoints);

    [[nodiscard]] Result<std::unique_ptr<INavigationQueryBackend>> MakeRecastDetourNavigationQueryBackend(
        const RecastDetourProviderCreateInfo &info, NavMeshPtr mesh, std::vector<QuerySlot> slots, std::vector<Math::Vec3> vertices,
        std::vector<GroundedNavigationPolygon> polygons);
}  // namespace Horo::Navigation
