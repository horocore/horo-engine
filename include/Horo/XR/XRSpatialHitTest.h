#pragma once

/** @file XRSpatialHitTest.h @brief Bounded World-space hit tests over admitted plane observations. */
#include "Horo/XR/XRPlaneTracking.h"

#include <optional>

namespace Horo::XR {
    /** @brief Exact live plane scope and finite World-space ray segment for one read-only query. */
    struct XRSpatialHitRequest final {
        XRPlaneScope scope;
        Math::Ray ray;
        XRRuntimeTime now;   /**< Consumer's query time. A live host also applies its authoritative tracking cutoff. */
        Duration maximumAge; /**< Maximum age allowed by the consumer, in the runtime clock domain. */
        std::uint32_t maximumHits{};
        std::optional<XRRuntimeTime> expectedSampledAt{}; /**< Pins a previously observed sample when validating a placement preview. */
    };

    /** @brief One bounded surface hit. A hit carries no Scene mutation or placement authority. */
    struct XRSpatialHit final {
        XRPlaneId plane;
        Math::RayHit intersection;
        XRPlaneClassification classification{XRPlaneClassification::Unknown};
        XRTrackingConfidence confidence{XRTrackingConfidence::None};
    };

    /** @brief Complete immutable hit batch with the source scope and observation revision. */
    class XRSpatialHits final {
    public:
        /** @return Purpose-bound World scope used by each hit. */
        [[nodiscard]] const XRPlaneScope &Source() const noexcept {
            return source_;
        }

        /** @return Exact runtime timestamp of the observed source batch. */
        [[nodiscard]] XRRuntimeTime SampledAt() const noexcept {
            return sampledAt_;
        }

        /** @return All matches ordered by distance, then portable plane identity. */
        [[nodiscard]] std::span<const XRSpatialHit> Hits() const noexcept {
            return std::span{hits_}.first(count_);
        }

    private:
        friend Result<XRSpatialHits> QueryXRPlaneHits(const XRPlaneSnapshot &, const XRSpatialHitRequest &);
        XRPlaneScope source_;
        XRRuntimeTime sampledAt_;
        std::array<XRSpatialHit, XRPlaneSnapshot::MaximumPlanes> hits_{};
        std::uint32_t count_{};
    };

    /** @brief Intersects a validated ray segment with each bounded observed rectangle.
     * @pre The caller revalidates current permission and World scope with the observation owner before obtaining the snapshot.
     * @param snapshot Current immutable observations from the exact host publication.
     * @param request Expected scope and age, geometric ray, and maximum complete hit count.
     * @return Complete ordered hits or typed stale, malformed, or over-capacity failure.
     * @note This operation does not read a native provider or submit a gameplay, Physics, or editor placement. */
    [[nodiscard]] Result<XRSpatialHits> QueryXRPlaneHits(const XRPlaneSnapshot &snapshot, const XRSpatialHitRequest &request);
}  // namespace Horo::XR
