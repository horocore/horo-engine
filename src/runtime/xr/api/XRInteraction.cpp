#include "Horo/XR/XRInteraction.h"

#include <cmath>

namespace Horo::XR {
    namespace {
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Resolves one exact controller pose or hand joint through the canonical snapshot queries. */
        Result<XRPoseSample> ResolvePose(const XRTrackingSnapshot &snapshot, const XRInteractionCutoff &cutoff,
                                         const XRInteractionProbe &probe) {
            if (const auto *controller = std::get_if<XRControllerInteractionPose>(&probe.pose)) {
                using enum XRControllerInteractionPose;
                if (*controller != Aim && *controller != Grip)
                    return Reject<XRPoseSample>(XRErrors::OperationInvalid);
                const auto kind = *controller == Aim ? XRTrackedPoseKind::Aim : XRTrackedPoseKind::Grip;
                return QueryXRTrackedPose(snapshot, probe.device, kind, cutoff.session, cutoff.tracking, cutoff.origin);
            }
            const auto joint =
                QueryXRHandJoint(snapshot, probe.device, std::get<XRHandJoint>(probe.pose), cutoff.session, cutoff.tracking, cutoff.origin);
            if (joint.HasError())
                return Result<XRPoseSample>::Failure(joint.ErrorValue());
            return Result<XRPoseSample>::Success(*joint.Value().sample);
        }

        /** @brief Checks explicit bounds and the required position/orientation of a live sample. */
        Result<XRInteractionShape> MakeGeometry(const XRPoseSample &sample, const XRInteractionProbe &probe) {
            const auto &components = sample.Components();
            if (probe.minimumConfidence <= XRTrackingConfidence::None || probe.minimumConfidence >= XRTrackingConfidence::Count)
                return Reject<XRInteractionShape>(XRErrors::OperationInvalid);
            if (!components.positionMeters.value || components.confidence < probe.minimumConfidence)
                return Reject<XRInteractionShape>(XRErrors::OperationUnavailable);
            const auto point = *components.positionMeters.value;
            if (const auto *ray = std::get_if<XRInteractionRayProbe>(&probe.shape)) {
                if (!std::isfinite(ray->maximumDistanceMeters) || ray->maximumDistanceMeters <= 0 || !Math::IsFinite(ray->localDirection) ||
                    Math::LengthSquared(ray->localDirection) <= 0)
                    return Reject<XRInteractionShape>(XRErrors::OperationInvalid);
                if (!components.orientation.value)
                    return Reject<XRInteractionShape>(XRErrors::OperationUnavailable);
                const auto direction = components.orientation.value->Rotate(ray->localDirection);
                const auto segment = Math::TryMakeRay(point, direction, 0.0F, ray->maximumDistanceMeters);
                if (segment.HasError())
                    return Reject<XRInteractionShape>(XRErrors::OperationInvalid);
                return Result<XRInteractionShape>::Success(segment.Value());
            }
            if (const auto *proximity = std::get_if<XRInteractionProximityProbe>(&probe.shape)) {
                if (!std::isfinite(proximity->radiusMeters) || proximity->radiusMeters <= 0)
                    return Reject<XRInteractionShape>(XRErrors::OperationInvalid);
                return Result<XRInteractionShape>::Success(Math::BoundingSphere{point, proximity->radiusMeters});
            }
            return Result<XRInteractionShape>::Success(point);
        }
    }  // namespace

    /** @copydoc DescribeXRInteractionCutoff */
    XRInteractionCutoff DescribeXRInteractionCutoff(const XRTrackingSnapshot &snapshot) noexcept {
        return {snapshot.Session(), snapshot.Revision(), snapshot.WorldOriginRevision(), snapshot.SampledAt(), snapshot.BindingRevision()};
    }

    /** @copydoc BuildXRInteractionCandidates */
    Result<XRInteractionCandidates> BuildXRInteractionCandidates(const XRTrackingSnapshot &snapshot, const XRInteractionCutoff &cutoff,
                                                                 const std::span<const XRInteractionProbe> probes) {
        using Outcome = Result<XRInteractionCandidates>;
        if (const auto current = ValidateXRTrackingSnapshot(snapshot, cutoff.session, cutoff.tracking, cutoff.origin); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (snapshot.SampledAt() != cutoff.sampledAt || snapshot.BindingRevision() != cutoff.bindingRevision)
            return Reject<XRInteractionCandidates>(XRErrors::TrackingSnapshotStale);
        if (probes.size() > XRInteractionCandidates::MaximumProbes)
            return Reject<XRInteractionCandidates>(XRErrors::CapacityExceeded);
        if (snapshot.Focus() != XRTrackingFocusState::Focused || snapshot.Visibility() != XRTrackingVisibilityState::Visible ||
            snapshot.State() != XRTrackingState::Active)
            return Reject<XRInteractionCandidates>(XRErrors::OperationUnavailable);

        XRInteractionCandidates result;
        result.cutoff_ = cutoff;
        for (const auto &probe : probes) {
            const auto sample = ResolvePose(snapshot, cutoff, probe);
            if (sample.HasError())
                return Outcome::Failure(sample.ErrorValue());
            if (sample.Value().Target().kind != XRSpaceKind::World)
                return Reject<XRInteractionCandidates>(XRErrors::OperationUnsupported);
            const auto shape = MakeGeometry(sample.Value(), probe);
            if (shape.HasError())
                return Outcome::Failure(shape.ErrorValue());
            result.values_[result.count_++] = {probe.device, probe.pose, sample.Value().Target(), sample.Value().Components().confidence,
                                               shape.Value()};
        }
        return Outcome::Success(std::move(result));
    }
}  // namespace Horo::XR
