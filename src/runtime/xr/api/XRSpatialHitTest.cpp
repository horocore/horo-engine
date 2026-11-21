#include "Horo/XR/XRSpatialHitTest.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace Horo::XR {
    namespace {
        /** @brief Restricts an infinite plane hit to the observation's finite local X/Z rectangle. */
        Result<std::optional<Math::RayHit>> Intersect(const Math::Ray &ray, const XRPlaneObservation &plane) {
            using Outcome = Result<std::optional<Math::RayHit>>;
            const auto normal = plane.orientation.Rotate({0.0F, 1.0F, 0.0F});
            const auto surface = Math::TryMakePlane(plane.centerMeters, normal);
            if (surface.HasError())
                return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            const auto intersection = Math::IntersectRayPlane(ray, surface.Value());
            if (intersection.HasError() || !intersection.Value())
                return intersection;
            const auto offset = intersection.Value()->position - plane.centerMeters;
            const auto localX = Math::Dot(offset, plane.orientation.Rotate({1.0F, 0.0F, 0.0F}));
            const auto localZ = Math::Dot(offset, plane.orientation.Rotate({0.0F, 0.0F, 1.0F}));
            if (!std::isfinite(localX) || !std::isfinite(localZ))
                return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            if (std::abs(localX) > plane.extentMeters.x * 0.5F || std::abs(localZ) > plane.extentMeters.y * 0.5F)
                return Outcome::Success(std::nullopt);
            return intersection;
        }
    }  // namespace

    /** @copydoc QueryXRPlaneHits */
    Result<XRSpatialHits> QueryXRPlaneHits(const XRPlaneSnapshot &snapshot, const XRSpatialHitRequest &request) {
        using Outcome = Result<XRSpatialHits>;
        if (request.scope != snapshot.Scope())
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (request.expectedSampledAt && *request.expectedSampledAt != snapshot.SampledAt())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        if (!snapshot.SampledAt().IsValid() || !request.now.IsValid() || request.now < snapshot.SampledAt() ||
            request.maximumAge <= Duration{})
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (request.now.Nanoseconds() - snapshot.SampledAt().Nanoseconds() > request.maximumAge.ToNanoseconds())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        if (request.maximumHits == 0 || request.maximumHits > XRPlaneSnapshot::MaximumPlanes)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto &segment = request.ray;
        const auto ray = Math::TryMakeRay(segment.origin, segment.direction, segment.minimumDistance, segment.maximumDistance);
        if (ray.HasError())
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));

        XRSpatialHits result;
        result.source_ = snapshot.Scope();
        result.sampledAt_ = snapshot.SampledAt();
        for (const auto &plane : snapshot.Planes()) {
            const auto hit = Intersect(ray.Value(), plane);
            if (hit.HasError())
                return Outcome::Failure(hit.ErrorValue());
            if (!hit.Value())
                continue;
            if (result.count_ == request.maximumHits)
                return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
            result.hits_[result.count_++] = {plane.id, *hit.Value(), plane.classification, plane.confidence};
        }
        std::ranges::sort(std::span{result.hits_}.first(result.count_), [](const XRSpatialHit &lhs, const XRSpatialHit &rhs) {
            return std::tie(lhs.intersection.distance, lhs.plane) < std::tie(rhs.intersection.distance, rhs.plane);
        });
        return Outcome::Success(std::move(result));
    }
}  // namespace Horo::XR
