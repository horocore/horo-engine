#include "Horo/XR/XRHandTracking.h"

#include "Horo/XR/XRErrors.h"

#include <cmath>

namespace Horo::XR {
    namespace {
        bool ValidJointShape(const XRHandJointRecord &record) noexcept {
            if (record.hand >= XRHandedness::Count || record.joint >= XRHandJoint::Count || !record.sample)
                return false;
            return !record.radiusMeters.has_value() || (std::isfinite(*record.radiusMeters) && *record.radiusMeters >= 0);
        }
    }  // namespace

    /** @copydoc ValidateXRHandJointRecord */
    Result<void> ValidateXRHandJointRecord(const XRHandJointRecord &record, const XRSessionId &session, const XRRuntimeTime sampledAt,
                                           const XRWorldOriginRevision origin) {
        if (auto identity = ValidateXRSessionObject(record.device, session); identity.HasError())
            return identity;
        if (!ValidJointShape(record))
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        const auto &sample = *record.sample;
        if (sample.Session() != session || sample.Purpose() != XRPosePurpose::SimulationInput || sample.Time().runtimeSample != sampledAt)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        const auto pose = XRPoseSample::Create({.session = sample.Session(),
                                                .source = sample.Source(),
                                                .target = sample.Target(),
                                                .purpose = sample.Purpose(),
                                                .time = sample.Time(),
                                                .components = sample.Components()},
                                               session, origin);
        return pose.HasError() ? Result<void>::Failure(pose.ErrorValue()) : Result<void>::Success();
    }
}  // namespace Horo::XR
