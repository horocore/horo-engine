#include "Horo/XR/XRHandTracking.h"

#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRTrackingSnapshot.h"

#include <algorithm>
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

    /** @copydoc XRContactPinchGesturePolicy::Classify */
    std::optional<std::pair<XRHandGestureKind, XRTrackingConfidence>> XRContactPinchGesturePolicy::Classify(
        std::span<const XRHandJointRecord> joints) const noexcept {
        const auto thumb = std::ranges::find(joints, XRHandJoint::ThumbTip, &XRHandJointRecord::joint);
        const auto index = std::ranges::find(joints, XRHandJoint::IndexTip, &XRHandJointRecord::joint);
        const auto admitsTip = [&](const auto &joint) {
            return joint != joints.end() && joint->radiusMeters && *joint->radiusMeters > 0 && joint->sample &&
                   joint->sample->Components().confidence == XRTrackingConfidence::High &&
                   joint->sample->Components().positionMeters.validity == XRPoseComponentValidity::Tracked;
        };
        if (!admitsTip(thumb) || !admitsTip(index) || thumb->sample->Target() != index->sample->Target())
            return std::nullopt;
        const auto separation = *thumb->sample->Components().positionMeters.value - *index->sample->Components().positionMeters.value;
        const float contact = *thumb->radiusMeters + *index->radiusMeters;
        const float contactSquared = contact * contact;
        const float distanceSquared = Math::LengthSquared(separation);
        if (!std::isfinite(contactSquared) || !std::isfinite(distanceSquared) || distanceSquared > contactSquared)
            return std::nullopt;
        return std::pair{XRHandGestureKind::Pinch, XRTrackingConfidence::High};
    }

    /** @copydoc DeriveXRHandGestures */
    Result<XRHandGestureCandidates> DeriveXRHandGestures(const XRTrackingSnapshot &snapshot, const XRSessionId &session,
                                                         XRTrackingSnapshotRevision revision, XRWorldOriginRevision origin,
                                                         const IXRHandGesturePolicy &policy) {
        using Outcome = Result<XRHandGestureCandidates>;
        if (auto current = ValidateXRTrackingSnapshot(snapshot, session, revision, origin); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        XRHandGestureCandidates output;
        if (snapshot.Focus() != XRTrackingFocusState::Focused || snapshot.State() != XRTrackingState::Active)
            return Outcome::Success(output);
        auto joints = snapshot.HandJoints();
        while (!joints.empty()) {
            const auto &source = joints.front();
            const auto end = std::ranges::find_if(joints, [&](const XRHandJointRecord &joint) {
                return joint.device != source.device;
            });
            const auto count = static_cast<std::size_t>(end - joints.begin());
            if (const auto classified = policy.Classify(joints.first(count))) {
                const auto [kind, confidence] = *classified;
                if (kind >= XRHandGestureKind::Count || confidence == XRTrackingConfidence::None ||
                    confidence >= XRTrackingConfidence::Count || output.count_ == output.values_.size())
                    return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                output.values_[output.count_++] = {source.device, source.hand, kind, confidence};
            }
            joints = joints.subspan(count);
        }
        return Outcome::Success(output);
    }
}  // namespace Horo::XR
