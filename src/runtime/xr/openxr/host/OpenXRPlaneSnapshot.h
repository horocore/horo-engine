#pragma once

/** @file OpenXRPlaneSnapshot.h @brief Bounded native plane snapshot projection on the OpenXR owner lane. */
#include "Horo/XR/XRPlaneTracking.h"
#include "OpenXRNativeCensus.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief One tracked plane while its native snapshot and authorized context are live. */
    struct NativeSpatialPlane final {
        XrSpatialEntityIdEXT entity;
        Math::Vec3 centerMeters;
        Math::Quaternion orientation;
        Math::Vec2 extentMeters;
        XRPlaneClassification classification;
        XRTrackingConfidence confidence;
    };

    /** @brief Complete tracked rectangle evidence at one snapshot's calibrated World transform. */
    class NativeSpatialPlaneBatch final {
    public:
        /** @return Bounded observations in increasing native entity ID order. */
        [[nodiscard]] std::span<const NativeSpatialPlane> Planes() const noexcept {
            return std::span{planes_}.first(count_);
        }

    private:
        friend Result<NativeSpatialPlaneBatch> ReadNativeSpatialPlanes(XrSpatialSnapshotEXT, PFN_xrQuerySpatialComponentDataEXT,
                                                                       NativePlaneCensus, std::uint32_t);
        std::array<NativeSpatialPlane, XRPlaneSnapshot::MaximumPlanes> planes_{};
        std::uint32_t count_{};
    };

    /** @brief Copies current tracked rectangles and optional labels from one complete native snapshot.
     * @pre The caller holds its exact consented context and has resolved the snapshot in the active calibrated World space.
     * @param snapshot Borrowed handle, destroyed by the native owner.
     * @param query Official query dispatch from the exact owner.
     * @param census Admitted bounded component evidence for that owner.
     * @param maximumPlanes Finite candidate count admitted before accessing geometry.
     * @return A complete batch or typed unavailable, capacity, or malformed native result. Never reads polygons.
     */
    [[nodiscard]] Result<NativeSpatialPlaneBatch> ReadNativeSpatialPlanes(XrSpatialSnapshotEXT snapshot,
                                                                          PFN_xrQuerySpatialComponentDataEXT query,
                                                                          NativePlaneCensus census, std::uint32_t maximumPlanes);
}  // namespace Horo::XR::OpenXRInternal
