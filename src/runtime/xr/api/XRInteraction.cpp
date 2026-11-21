#include "Horo/XR/XRInteraction.h"

#include <algorithm>
#include <cmath>

namespace Horo::XR {
    namespace {
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Confirms the complete committed pose and input provenance. */
        Result<void> ValidateInteractionCutoff(const XRTrackingSnapshot &snapshot, const XRInteractionCutoff &cutoff) {
            if (const auto current = ValidateXRTrackingSnapshot(snapshot, cutoff.session, cutoff.tracking, cutoff.origin);
                current.HasError())
                return current;
            if (snapshot.SampledAt() != cutoff.sampledAt || snapshot.BindingRevision() != cutoff.bindingRevision)
                return Reject<void>(XRErrors::TrackingSnapshotStale);
            return Result<void>::Success();
        }

        /** @brief Checks whether the committed snapshot can supply active interaction evidence. */
        bool InteractionTrackingAvailable(const XRTrackingSnapshot &snapshot) noexcept {
            return snapshot.Focus() == XRTrackingFocusState::Focused && snapshot.Visibility() == XRTrackingVisibilityState::Visible &&
                   snapshot.State() == XRTrackingState::Active;
        }

        /** @brief Validates the current metric Stage bounds and their reference identities. */
        Result<XRStageBounds> ValidateStageReference(const XRReferenceSpaceSnapshot &referenceSpaces, const XRInteractionCutoff &cutoff) {
            if (referenceSpaces.session != cutoff.session)
                return Reject<XRStageBounds>(XRErrors::IdentityStale);
            if (!referenceSpaces.stage)
                return Reject<XRStageBounds>(XRErrors::OperationUnsupported);
            const auto &stage = *referenceSpaces.stage;
            const auto &view = referenceSpaces.view;
            if (!view.id.IsValid() || view.id.session != cutoff.session || !stage.id.IsValid() || stage.id.session != cutoff.session ||
                view.kind != XRSpaceKind::View || stage.kind != XRSpaceKind::Stage)
                return Reject<XRStageBounds>(XRErrors::CoordinateSpaceIncompatible);
            if (view.worldOriginRevision != cutoff.origin || stage.worldOriginRevision != cutoff.origin)
                return Reject<XRStageBounds>(XRErrors::OriginRevisionStale);
            if (!referenceSpaces.stageBounds)
                return Reject<XRStageBounds>(XRErrors::OperationUnavailable);
            const auto bounds = *referenceSpaces.stageBounds;
            if (!std::isfinite(bounds.widthMeters) || !std::isfinite(bounds.depthMeters) || bounds.widthMeters <= 0.0F ||
                bounds.depthMeters <= 0.0F)
                return Reject<XRStageBounds>(XRErrors::OperationInvalid);
            return Result<XRStageBounds>::Success(bounds);
        }

        /** @brief Classifies the shortest horizontal clearance from the measured rectangle. */
        XRStageBoundaryProximity BoundaryProximity(const Math::Vec3 &point, XRStageBounds bounds, float warningDistanceMeters) noexcept {
            const float clearance = std::min(bounds.widthMeters * 0.5F - std::abs(point.x), bounds.depthMeters * 0.5F - std::abs(point.z));
            if (clearance < 0.0F)
                return XRStageBoundaryProximity::Outside;
            return clearance <= warningDistanceMeters ? XRStageBoundaryProximity::Near : XRStageBoundaryProximity::Inside;
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

    /** @copydoc QueryXRStageBoundary */
    Result<XRStageBoundaryProximity> QueryXRStageBoundary(const XRTrackingSnapshot &snapshot, const XRInteractionCutoff &cutoff,
                                                          const XRReferenceSpaceSnapshot &referenceSpaces, float warningDistanceMeters) {
        using Outcome = Result<XRStageBoundaryProximity>;
        if (!std::isfinite(warningDistanceMeters) || warningDistanceMeters < 0.0F)
            return Reject<XRStageBoundaryProximity>(XRErrors::OperationInvalid);
        if (const auto current = ValidateInteractionCutoff(snapshot, cutoff); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto bounds = ValidateStageReference(referenceSpaces, cutoff);
        if (bounds.HasError())
            return Outcome::Failure(bounds.ErrorValue());
        if (!InteractionTrackingAvailable(snapshot))
            return Reject<XRStageBoundaryProximity>(XRErrors::OperationUnavailable);

        const auto found = std::ranges::find_if(snapshot.Poses(), [&](const XRTrackedPoseRecord &pose) {
            return pose.kind == XRTrackedPoseKind::Head && pose.sample.Source() == referenceSpaces.view &&
                   pose.sample.Target() == *referenceSpaces.stage;
        });
        if (found == snapshot.Poses().end() || found->sample.Components().positionMeters.validity != XRPoseComponentValidity::Tracked)
            return Reject<XRStageBoundaryProximity>(XRErrors::OperationUnavailable);
        return Outcome::Success(BoundaryProximity(*found->sample.Components().positionMeters.value, bounds.Value(), warningDistanceMeters));
    }

    /** @copydoc ClassifyXRStageBoundary */
    Result<XRStageBoundaryProximity> ClassifyXRStageBoundary(const XRPoseSample &head, const XRReferenceSpaceSnapshot &referenceSpaces,
                                                             XRRuntimeTime sampledAt, float warningDistanceMeters) {
        using Outcome = Result<XRStageBoundaryProximity>;
        if (!std::isfinite(warningDistanceMeters) || warningDistanceMeters < 0.0F)
            return Reject<XRStageBoundaryProximity>(XRErrors::OperationInvalid);
        if (head.Purpose() != XRPosePurpose::SimulationInput || !sampledAt.IsValid() || head.Time().runtimeSample != sampledAt)
            return Reject<XRStageBoundaryProximity>(XRErrors::TrackingSnapshotStale);
        const XRInteractionCutoff cutoff{.session = head.Session(), .origin = head.Target().worldOriginRevision, .sampledAt = sampledAt};
        const auto bounds = ValidateStageReference(referenceSpaces, cutoff);
        if (bounds.HasError())
            return Outcome::Failure(bounds.ErrorValue());
        if (head.Source() != referenceSpaces.view || head.Target() != *referenceSpaces.stage)
            return Reject<XRStageBoundaryProximity>(XRErrors::CoordinateSpaceIncompatible);
        const auto &position = head.Components().positionMeters;
        if (position.validity != XRPoseComponentValidity::Tracked)
            return Reject<XRStageBoundaryProximity>(XRErrors::OperationUnavailable);
        return Outcome::Success(BoundaryProximity(*position.value, bounds.Value(), warningDistanceMeters));
    }

    /** @copydoc BuildXRInteractionCandidates */
    Result<XRInteractionCandidates> BuildXRInteractionCandidates(const XRTrackingSnapshot &snapshot, const XRInteractionCutoff &cutoff,
                                                                 const std::span<const XRInteractionProbe> probes) {
        using Outcome = Result<XRInteractionCandidates>;
        if (const auto current = ValidateInteractionCutoff(snapshot, cutoff); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (probes.size() > XRInteractionCandidates::MaximumProbes)
            return Reject<XRInteractionCandidates>(XRErrors::CapacityExceeded);
        if (!InteractionTrackingAvailable(snapshot))
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
