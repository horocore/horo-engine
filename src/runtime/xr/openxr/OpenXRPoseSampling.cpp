#include "host/OpenXRActionBindings.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves exact native tracking query errors. */
        Error PoseFailure(const char *phase, const XrResult result) {
            return MakeError(XRErrors::OperationUnavailable,
                             std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Projects explicit platform denial without admitting any gaze payload. */
        Error GazeFailure(const char *phase, const XrResult result) {
            return result == XR_ERROR_PERMISSION_INSUFFICIENT ? MakeError(XRErrors::PermissionDenied) : PoseFailure(phase, result);
        }

        /** @brief Maps native validity independently; absent components are never replaced by default transforms. */
        XRPoseComponents Components(const XrSpaceLocation &location, const XrSpaceVelocity &velocity) {
            using enum Horo::XR::XRPoseComponentValidity;
            const auto flags = location.locationFlags;
            const bool position = (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            const bool orientation = (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            const bool positionTracked = (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
            const bool orientationTracked = (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
            XRPoseComponents result;
            if (position)
                result.positionMeters = {{Math::Vec3{location.pose.position.x, location.pose.position.y, location.pose.position.z}},
                                         positionTracked ? Tracked : Inferred};
            if (orientation)
                result.orientation = {{Math::Quaternion{location.pose.orientation.x, location.pose.orientation.y,
                                                        location.pose.orientation.z, location.pose.orientation.w}},
                                      orientationTracked ? Tracked : Inferred};
            if (position && (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
                result.linearVelocityMetersPerSecond = {{Math::Vec3{velocity.linearVelocity.x, velocity.linearVelocity.y,
                                                                    velocity.linearVelocity.z}},
                                                        Inferred};
            if (orientation && (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0)
                result.angularVelocityRadiansPerSecond = {{Math::Vec3{velocity.angularVelocity.x, velocity.angularVelocity.y,
                                                                      velocity.angularVelocity.z}},
                                                          Inferred};
            if (position || orientation)
                result.confidence = positionTracked && orientationTracked ? XRTrackingConfidence::High : XRTrackingConfidence::Low;
            if (position && orientation)
                result.loss = XRTrackingLossState::None;
            else if (position)
                result.loss = XRTrackingLossState::OrientationLost;
            else if (orientation)
                result.loss = XRTrackingLossState::PositionLost;
            return result;
        }

        /** @brief Validates bounded runtime-issued source provenance before the first native callback. */
        Result<void> ValidateDevices(const std::span<const XRTrackedDeviceRecord> devices, const XRSessionId &owner) {
            for (std::size_t index = 0; index < devices.size(); ++index) {
                if (auto valid = ValidateXRSessionObject(devices[index].id, owner); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                if (devices[index].role >= XRTrackedDeviceRole::Count || devices[index].state >= XRTrackedDeviceState::Count ||
                    std::ranges::find(devices.first(index), devices[index].role, &XRTrackedDeviceRecord::role) !=
                        devices.first(index).end())
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Validates the exact native location and independent known validity bits. */
        Result<void> ValidateLocation(const XrSpaceLocation &location, const void *chain) {
            constexpr XrSpaceLocationFlags known = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                   XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            if (location.type != XR_TYPE_SPACE_LOCATION || location.next != chain || (location.locationFlags & ~known) != 0 ||
                ((location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0 &&
                 (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) ||
                ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0 &&
                 (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0))
                return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            return Result<void>::Success();
        }

        /** @brief Validates the native velocity chain only for consumers that requested it. */
        Result<void> ValidateLocation(const XrSpaceLocation &location, const XrSpaceVelocity &velocity) {
            constexpr XrSpaceVelocityFlags known = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            if (velocity.type != XR_TYPE_SPACE_VELOCITY || velocity.next || (velocity.velocityFlags & ~known) != 0)
                return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            return ValidateLocation(location, &velocity);
        }

        /** @brief Decodes a fully tracked rigid calibration pose without normalizing invalid native data. */
        Result<Math::Transform> DecodeCalibrationPose(const XrSpaceLocation &location, const XrSpaceVelocity &velocity) {
            using Pose = Result<Math::Transform>;
            if (const auto valid = ValidateLocation(location, velocity); valid.HasError())
                return Pose::Failure(valid.ErrorValue());
            constexpr XrSpaceLocationFlags complete = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            if (location.locationFlags != complete)
                return Pose::Failure(MakeError(XRErrors::OperationUnavailable, "OpenXR calibration requires a fully tracked relation."));
            const auto &native = location.pose;
            const Math::Vec3 position{native.position.x, native.position.y, native.position.z};
            const Math::Quaternion orientation{native.orientation.x, native.orientation.y, native.orientation.z, native.orientation.w};
            const auto lengthSquared = Math::LengthSquared(Math::Vec4{orientation.x, orientation.y, orientation.z, orientation.w});
            constexpr float UnitTolerance = 1.0e-4F;
            if (!Math::IsFinite(position) || !std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0F) > UnitTolerance)
                return Pose::Failure(MakeError(XRErrors::PoseInvalid));
            return Pose::Success({position, orientation});
        }

        /** @brief Validates complete stereo ABI, independent component flags and asymmetric view ranges before any publication. */
        Result<void> ValidatePresentationViews(const NativePresentationViews &output, const XrViewState &state,
                                               std::uint32_t expectedViews) {
            if (constexpr XrViewStateFlags known = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                                   XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
                state.type != XR_TYPE_VIEW_STATE || state.next || output.count != expectedViews || (state.viewStateFlags & ~known) != 0 ||
                ((state.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) &&
                 !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) ||
                ((state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) &&
                 !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)))
                return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            for (const auto &view : std::span{output.views}.first(output.count)) {
                constexpr float rightAngle = 1.57079632679489661923F;
                if (const auto &fov = view.fov; view.type != XR_TYPE_VIEW || view.next || !std::isfinite(fov.angleLeft) ||
                                                !std::isfinite(fov.angleRight) || !std::isfinite(fov.angleDown) ||
                                                !std::isfinite(fov.angleUp) || fov.angleLeft <= -rightAngle ||
                                                fov.angleRight >= rightAngle || fov.angleDown <= -rightAngle || fov.angleUp >= rightAngle ||
                                                fov.angleLeft >= fov.angleRight || fov.angleDown >= fov.angleUp)
                    return Result<void>::Failure(MakeError(XRErrors::ViewPlanInvalid));
                if ((state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                    !Math::IsFinite(Math::Vec3{view.pose.position.x, view.pose.position.y, view.pose.position.z}))
                    return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
                if ((state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                    Math::Quaternion{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w}
                        .TryNormalized()
                        .HasError())
                    return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::Tracking::Locate */
    Result<XRPoseDescriptor> OpenXRActionBindings::Tracking::Locate(const TrackingContext &context, const XrSpace source,
                                                                    const XRCoordinateSpace &identity, const LocationTarget &target,
                                                                    const XRTimeEvidence &time) const {
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
        if (const auto result = context.DispatchTable().locate(source, target.space, time.runtimeSample.Nanoseconds(), &location);
            result != XR_SUCCESS)
            return Result<XRPoseDescriptor>::Failure(PoseFailure("locate tracking space", result));
        if (auto valid = ValidateLocation(location, velocity); valid.HasError())
            return Result<XRPoseDescriptor>::Failure(valid.ErrorValue());
        const XRPoseDescriptor descriptor{context.Session(),
                                          identity,
                                          target.identity,
                                          XRPosePurpose::SimulationInput,
                                          time,
                                          Components(location, velocity)};
        const auto validated = XRPoseSample::Create(descriptor, context.Session(), request->origin);
        return validated.HasError() ? Result<XRPoseDescriptor>::Failure(validated.ErrorValue())
                                    : Result<XRPoseDescriptor>::Success(descriptor);
    }

    /** @copydoc OpenXRActionBindings::SampleConsentedGaze */
    Result<std::optional<XRPoseSample>> OpenXRActionBindings::SampleConsentedGaze(const XRSessionId &session, const XRTimeEvidence &cutoff,
                                                                                  const Duration ageLimit, const bool &consent) const {
        using Outcome = Result<std::optional<XRPoseSample>>;
        if (auto lane = ControlLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (sampling_ || syncSequence_ == 0 || tracking_.gazeSequence == syncSequence_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{sampling_};
        if (const auto current = Current(session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!tracking_.request || tracking_.gazeSpace == XR_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!consent)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        if (!cutoff.runtimeSample.IsValid() || !cutoff.simulation || !cutoff.simulation->IsValid() || cutoff.renderPrediction ||
            ageLimit <= Duration{})
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        tracking_.gazeSequence = syncSequence_;
        const XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, tracking_.gazeAction, XR_NULL_PATH};
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        const auto queried = dispatch_.poseState(native_.session, &info, &state);
        if (!consent)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        if (queried != XR_SUCCESS)
            return Outcome::Failure(GazeFailure("query gaze activity", queried));
        if (state.type != XR_TYPE_ACTION_STATE_POSE || state.next || state.isActive > XR_TRUE)
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        if (state.isActive == XR_FALSE)
            return Outcome::Success(std::nullopt);
        if (const auto current = ValidateOwner(); current.HasError())
            return Outcome::Failure(current.ErrorValue());

        XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &sampleTime};
        const auto located = dispatch_.locate(tracking_.gazeSpace, tracking_.base, cutoff.runtimeSample.Nanoseconds(), &location);
        if (!consent)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        if (located != XR_SUCCESS)
            return Outcome::Failure(GazeFailure("locate gaze space", located));
        if (sampleTime.type != XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT || sampleTime.next)
            return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
        if (auto valid = ValidateLocation(location, &sampleTime); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        constexpr auto position = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        constexpr auto orientation = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const auto tracked = location.locationFlags & (position | orientation);
        if (tracked == 0)
            return Outcome::Success(std::nullopt);
        if (tracked != (position | orientation))
            return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
        const auto when = XRRuntimeTime::Create(sampleTime.time);
        if (when.HasError() || when.Value() > cutoff.runtimeSample ||
            cutoff.runtimeSample.Nanoseconds() - when.Value().Nanoseconds() > ageLimit.ToNanoseconds())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        if (const auto current = ValidateOwner(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        // Gaze velocity is not part of the granted pointing purpose.
        const auto components = Components(location, XrSpaceVelocity{XR_TYPE_SPACE_VELOCITY});
        auto sample = XRPoseSample::Create({session,
                                            tracking_.gazeIdentity,
                                            tracking_.baseIdentity,
                                            XRPosePurpose::SimulationInput,
                                            {when.Value(), cutoff.simulation, {}},
                                            components},
                                           session, tracking_.request->origin);
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        return Outcome::Success(std::move(sample).Value());
    }

    /** @copydoc OpenXRActionBindings::Tracking::LocateAction */
    Result<NativePoseObservation> OpenXRActionBindings::Tracking::LocateAction(const TrackingContext &context, const PoseAction &action,
                                                                               const LocationTarget &target,
                                                                               const XRTimeEvidence &time) const {
        using Outcome = Result<NativePoseObservation>;
        const std::size_t role = action.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
        const XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action.native, context.Hands()[role]};
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (const auto result = context.DispatchTable().poseState(context.Native().session, &info, &state); result != XR_SUCCESS)
            return Outcome::Failure(PoseFailure("query pose action", result));
        if (state.type != XR_TYPE_ACTION_STATE_POSE || state.next != nullptr || (state.isActive != XR_TRUE && state.isActive != XR_FALSE))
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        XRPoseDescriptor descriptor{context.Session(), action.identity, target.identity, XRPosePurpose::SimulationInput, time, {}};
        const bool active = state.isActive == XR_TRUE;
        if (active) {
            auto pose = Locate(context, action.space, action.identity, target, time);
            if (pose.HasError())
                return Outcome::Failure(pose.ErrorValue());
            descriptor = pose.Value();
        }
        return Outcome::Success({action.role, action.kind, active, descriptor});
    }

    /** @copydoc OpenXRActionBindings::Tracking::ObserveStageBoundary */
    std::optional<XRStageBoundaryProximity> OpenXRActionBindings::Tracking::ObserveStageBoundary(const TrackingContext &context,
                                                                                                 const XRTimeEvidence &time) const {
        if (!request->stageBoundaryWarningMeters || !stageBounds)
            return std::nullopt;
        const bool baseIsStage = request->base == XRSpaceKind::Stage;
        const auto stage = baseIsStage ? LocationTarget{base, baseIdentity} : LocationTarget{alternate, alternateIdentity};
        if (!stage.identity.id.IsValid() || stage.space == XR_NULL_HANDLE)
            return std::nullopt;
        const auto location = Locate(context, view, viewIdentity, stage, time);
        if (location.HasError())
            return std::nullopt;
        const auto sample = XRPoseSample::Create(location.Value(), context.Session(), request->origin);
        if (sample.HasError())
            return std::nullopt;
        const XRReferenceSpaceSnapshot references{context.Session(), viewIdentity, baseIsStage ? alternateIdentity : baseIdentity,
                                                  stage.identity, stageBounds};
        const auto proximity =
            ClassifyXRStageBoundary(sample.Value(), references, time.runtimeSample, *request->stageBoundaryWarningMeters);
        return proximity.HasValue() ? std::optional{proximity.Value()} : std::nullopt;
    }

    /** @copydoc OpenXRActionBindings::Tracking::AppendHeadObservation */
    Result<void> OpenXRActionBindings::Tracking::AppendHeadObservation(const TrackingContext &context, const LocationTarget &target,
                                                                       const XRTimeEvidence &time, NativePoseObservations &samples) const {
        auto pose = Locate(context, view, viewIdentity, target, time);
        if (pose.HasError())
            return Result<void>::Failure(pose.ErrorValue());
        const auto &components = pose.Value().components;
        const bool observed = components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                              components.orientation.validity != XRPoseComponentValidity::Invalid;
        samples.records_[samples.count_++] = {XRTrackedDeviceRole::Head, XRTrackedPoseKind::Head, observed, pose.Value()};
        return Result<void>::Success();
    }

    namespace {
        /** @brief Filters only the retained issuer-owned device generation and capability before native sampling. */
        bool AdmitsTrackingSource(const std::span<const XRTrackedDeviceRecord> devices, const bool filter, const XRTrackedDeviceRole role,
                                  const XRTrackedDeviceCapability capability) {
            if (!filter)
                return true;
            const auto device = std::ranges::find(devices, role, &XRTrackedDeviceRecord::role);
            return device != devices.end() && device->state != XRTrackedDeviceState::Disconnected && device->capabilities.Has(capability);
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::CollectPoseObservations */
    Result<NativePoseObservations> OpenXRActionBindings::CollectPoseObservations(
        const XRSessionId &session, const XRTimeEvidence &time, const XRWorldOriginRevision origin,
        const std::optional<std::span<const XRTrackedDeviceRecord>> devices, const PoseReference reference) const {
        using Outcome = Result<NativePoseObservations>;
        if (auto lane = ControlLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (!active_ || sampling_ || !tracking_.request || syncSequence_ == 0 || tracking_.sampledSequence == syncSequence_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        tracking_.sampledSequence = syncSequence_;
        if (!time.runtimeSample.IsValid() || !time.simulation || !time.simulation->IsValid() || time.renderPrediction)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (origin != tracking_.request->origin)
            return Outcome::Failure(MakeError(XRErrors::OriginRevisionStale));
        if (devices && devices->size() > XRTrackingSnapshotHardLimits::MaximumDevices)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        std::array<XRTrackedDeviceRecord, XRTrackingSnapshotHardLimits::MaximumDevices> retained{};
        if (devices)
            std::ranges::copy(*devices, retained.begin());
        const auto currentDevices = std::span{retained}.first(devices ? devices->size() : 0);
        if (auto valid = ValidateDevices(currentDevices, owner_); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const Operation operation{sampling_};
        if (const auto current = Current(session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto target = reference == PoseReference::CalibratedWorld ? Tracking::LocationTarget{tracking_.world, tracking_.worldIdentity}
                                                                        : Tracking::LocationTarget{tracking_.base, tracking_.baseIdentity};
        if (target.space == XR_NULL_HANDLE || !target.identity.id.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        NativePoseObservations samples;
        if (AdmitsTrackingSource(currentDevices, devices.has_value(), XRTrackedDeviceRole::Head, XRTrackedDeviceCapability::HeadPose)) {
            if (auto observed = tracking_.AppendHeadObservation(TrackingContext{*this}, target, time, samples); observed.HasError())
                return Outcome::Failure(observed.ErrorValue());
            if (reference == PoseReference::CalibratedWorld)
                samples.stageBoundary_ = tracking_.ObserveStageBoundary(TrackingContext{*this}, time);
        }
        for (const auto &action : tracking_.actions) {
            if (const auto capability =
                    action.kind == XRTrackedPoseKind::Grip ? XRTrackedDeviceCapability::GripPose : XRTrackedDeviceCapability::AimPose;
                !AdmitsTrackingSource(currentDevices, devices.has_value(), action.role, capability))
                continue;
            auto pose = tracking_.LocateAction(TrackingContext{*this}, action, target, time);
            if (pose.HasError())
                return Outcome::Failure(pose.ErrorValue());
            samples.records_[samples.count_++] = pose.Value();
        }
        if (auto valid = session_->Validate(session); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success(samples);
    }

    /** @copydoc OpenXRActionBindings::ObservePoses */
    Result<NativePoseObservations> OpenXRActionBindings::ObservePoses(const XRSessionId &session, const XRTimeEvidence &time,
                                                                      const XRWorldOriginRevision origin) const {
        return CollectPoseObservations(session, time, origin, std::nullopt, PoseReference::Natural);
    }

    /** @copydoc OpenXRActionBindings::ObserveWorldPoses */
    Result<NativePoseObservations> OpenXRActionBindings::ObserveWorldPoses(const XRSessionId &session, const XRTimeEvidence &time,
                                                                           const XRWorldOriginRevision origin) const {
        return CollectPoseObservations(session, time, origin, std::nullopt, PoseReference::CalibratedWorld);
    }

    /** @copydoc OpenXRActionBindings::SamplePoses */
    Result<NativePoseSamples> OpenXRActionBindings::SamplePoses(const XRSessionId &session, const XRTimeEvidence &time,
                                                                const XRWorldOriginRevision origin,
                                                                const std::span<const XRTrackedDeviceRecord> devices) const {
        if (auto lane = ControlLane(); lane.HasError())
            return Result<NativePoseSamples>::Failure(lane.ErrorValue());
        // Retain the exact caller-issued generations before any native callback can reenter its publication owner.
        std::array<XRTrackedDeviceRecord, XRTrackingSnapshotHardLimits::MaximumDevices> retained{};
        if (devices.size() <= retained.size())
            std::ranges::copy(devices, retained.begin());
        const auto observed = CollectPoseObservations(session, time, origin, devices, PoseReference::Natural);
        if (observed.HasError())
            return Result<NativePoseSamples>::Failure(observed.ErrorValue());
        const auto currentDevices = std::span{retained}.first(devices.size());
        NativePoseSamples result;
        for (const auto &pose : observed.Value().Poses()) {
            const auto device = std::ranges::find(currentDevices, pose.role, &XRTrackedDeviceRecord::role);
            if (device == currentDevices.end())
                return Result<NativePoseSamples>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
            result.records_[result.count_++] = {device->id, pose.kind, pose.pose};
        }
        return Result<NativePoseSamples>::Success(result);
    }

    /** @copydoc OpenXRActionBindings::Tracking::PresentationReference */
    Result<XRPoseDescriptor> OpenXRActionBindings::Tracking::PresentationReference(const TrackingContext &context,
                                                                                   const XRTimeEvidence &time) const {
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation reference{XR_TYPE_SPACE_LOCATION, &velocity};
        if (const auto transformed = context.DispatchTable().locate(base, world, time.renderPrediction->Nanoseconds(), &reference);
            transformed != XR_SUCCESS)
            return Result<XRPoseDescriptor>::Failure(PoseFailure("locate presentation reference in World", transformed));
        if (auto valid = ValidateLocation(reference, velocity); valid.HasError())
            return Result<XRPoseDescriptor>::Failure(valid.ErrorValue());
        return Result<XRPoseDescriptor>::Success(
            {context.Session(), baseIdentity, worldIdentity, XRPosePurpose::PresentationPrediction, time, Components(reference, velocity)});
    }

    /** @copydoc OpenXRActionBindings::Tracking::ValidateCalibrationBinding */
    Result<void> OpenXRActionBindings::Tracking::ValidateCalibrationBinding(const XRCoordinateSpace &reference,
                                                                            const XRCoordinateSpace &target) const {
        if (!request || base == XR_NULL_HANDLE || world == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (reference != baseIdentity || target != worldIdentity || reference.worldOriginRevision != request->origin ||
            target.worldOriginRevision != request->origin)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::ObserveReferenceCalibration */
    Result<NativeReferenceCalibrationObservation> OpenXRActionBindings::ObserveReferenceCalibration(const XRCoordinateSpace reference,
                                                                                                    const XRCoordinateSpace world,
                                                                                                    const XRRuntimeTime now) const {
        using Outcome = Result<NativeReferenceCalibrationObservation>;
        if (const auto lane = ControlLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (!now.IsValid())
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (sampling_ || !active_ || !dispatch_.locate)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (const auto binding = tracking_.ValidateCalibrationBinding(reference, world); binding.HasError())
            return Outcome::Failure(binding.ErrorValue());

        const Operation operation{sampling_};
        const auto current = Current(reference.id.session);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
        const auto sampled = dispatch_.locate(tracking_.base, tracking_.world, now.Nanoseconds(), &location);
        if (sampled != XR_SUCCESS)
            return Outcome::Failure(PoseFailure("observe calibration reference in World", sampled));
        const auto fenced = Current(reference.id.session);
        if (fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        if (fenced.Value().incarnation != current.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto binding = tracking_.ValidateCalibrationBinding(reference, world); binding.HasError())
            return Outcome::Failure(binding.ErrorValue());
        const auto pose = DecodeCalibrationPose(location, velocity);
        return pose.HasError() ? Outcome::Failure(pose.ErrorValue()) : Outcome::Success({reference, world, now, pose.Value()});
    }

    /** @copydoc OpenXRActionBindings::LocatePresentation */
    Result<NativePresentationViews> OpenXRActionBindings::LocatePresentation(const XRSessionId &session, const XRTimeEvidence &time,
                                                                             const std::uint32_t expectedViews) const {
        using Outcome = Result<NativePresentationViews>;
        if (auto lane = ControlLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (sampling_ || !active_ || !dispatch_.locateViews || !tracking_.request || tracking_.base == XR_NULL_HANDLE ||
            tracking_.world == XR_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!time.runtimeSample.IsValid() || !time.renderPrediction || !time.renderPrediction->IsValid() || time.simulation)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (expectedViews != 2)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const Operation operation{sampling_};
        const auto current = Current(session);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        NativePresentationViews output;
        for (auto &view : output.views)
            view.type = XR_TYPE_VIEW;
        const XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                    time.renderPrediction->Nanoseconds(), tracking_.base};
        XrViewState state{XR_TYPE_VIEW_STATE};
        if (const auto located =
                dispatch_.locateViews(current.Value().session, &info, &state, expectedViews, &output.count, output.views.data());
            located != XR_SUCCESS)
            return Outcome::Failure(PoseFailure("locate primary views", located));
        if (auto valid = ValidatePresentationViews(output, state, expectedViews); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (auto fenced = Current(session); fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        auto reference = tracking_.PresentationReference(TrackingContext{*this}, time);
        if (reference.HasError())
            return Outcome::Failure(reference.ErrorValue());
        output.flags = state.viewStateFlags;
        for (std::uint32_t eye = 0; eye < expectedViews; ++eye) {
            const auto &identity = tracking_.eyeIdentities[eye];
            if (!identity.id.IsValid() || identity.id.session != session || identity.worldOriginRevision != tracking_.request->origin)
                return Outcome::Failure(MakeError(XRErrors::IdentityStale));
            output.eyes[eye] = identity;
        }
        output.reference = tracking_.baseIdentity;
        output.referenceToWorld = std::move(reference).Value();
        if (const auto validated = XRPoseSample::Create(output.referenceToWorld, owner_, tracking_.request->origin); validated.HasError())
            return Outcome::Failure(validated.ErrorValue());
        if (auto fenced = Current(session); fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        return Outcome::Success(std::move(output));
    }
}  // namespace Horo::XR::OpenXRInternal
