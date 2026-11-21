#include "Horo/Foundation/Assertions.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRInputClock.h"
#include "XRTrackingPublication.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    namespace {
        constexpr auto MaximumEyeGazeAge = Duration::FromMilliseconds(50);
        constexpr XR::XRPlaneLimits PlaneCapturePolicy{XR::XRPlaneSnapshot::MaximumPlanes, Duration::FromMilliseconds(250),
                                                       Duration::FromMilliseconds(1500)};

        /** @brief Maps the already-normalized native pose vocabulary to its exact issuer capability. */
        XR::XRTrackedDeviceCapability NativePoseCapability(const XR::XRTrackedPoseKind kind) {
            using enum XR::XRTrackedDeviceCapability;
            if (kind == XR::XRTrackedPoseKind::Head)
                return HeadPose;
            if (kind == XR::XRTrackedPoseKind::Grip)
                return GripPose;
            return AimPose;
        }

        /** @brief Preserves exactly one current pose's normalized provenance. */
        XR::XRPoseDescriptor Describe(const XR::XRPoseSample &pose) {
            return {pose.Session(), pose.Source(), pose.Target(), pose.Purpose(), pose.Time(), pose.Components()};
        }

        /** @brief Maps the fixed anatomical side order to distinct canonical device roles. */
        XR::XRTrackedDeviceRole HandRole(std::size_t hand) noexcept {
            return hand == 0 ? XR::XRTrackedDeviceRole::LeftHand : XR::XRTrackedDeviceRole::RightHand;
        }

        /** @brief Allows controller fallback only for failures known to affect the optional hand evidence. */
        std::optional<XR::XRCapabilityState> HandOutage(const Error &error) noexcept {
            using namespace XR;
            const auto &code = error.code.Value();
            if (code == XRErrors::PermissionDenied.code.Value())
                return XRCapabilityState::Denied;
            if (code == XRErrors::OperationUnsupported.code.Value())
                return XRCapabilityState::Unsupported;
            if (code == XRErrors::TrackingSnapshotInvalid.code.Value() || code == XRErrors::PoseInvalid.code.Value())
                return XRCapabilityState::Incompatible;
            return std::nullopt;
        }
    }  // namespace

    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRApplicationSession::WantsHands */
    bool OpenXRApplicationSession::WantsHands() const noexcept {
        const auto features = settings_.Features();
        const auto hand = std::ranges::find(features, XRCapability::HandTracking, &XRFeatureRequest::capability);
        return hand != features.end() && hand->kind != XRFeatureRequirementKind::Disabled;
    }

    /** @copydoc OpenXRApplicationSession::PrepareHands */
    Result<void> OpenXRApplicationSession::PrepareHands() {
        if (!WantsHands() || !system_ || system_->handTracking != XRCapabilityState::Available)
            return Result<void>::Success();
        // The same cutoff has room for both hands and its existing head/controller sources.
        constexpr auto sourceSlots = 1 + 2 + XRHandTrackingHardLimits::MaximumHands;
        if (settings_.Limits().maximumDevices < sourceSlots) {
            input_.handAvailability = XRCapabilityState::Unavailable;
            return Result<void>::Success();
        }
        input_.hands.emplace(*native_);
        const auto prepared = input_.hands->Prepare({*preflight_, selection_.attempt});
        if (prepared.HasError()) {
            if (auto retired = RetireHands(); retired.HasError())
                return retired;
            input_.handAvailability = prepared.ErrorValue().code.Value() == XRErrors::PermissionDenied.code.Value()
                                          ? XRCapabilityState::Denied
                                          : XRCapabilityState::Unavailable;
            return Result<void>::Success();
        }
        input_.handAvailability = XRCapabilityState::Available;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::RetireHands */
    Result<void> OpenXRApplicationSession::RetireHands() {
        input_.handCutoff.reset();
        input_.handSamplingBlocked.reset();
        if (input_.hands) {
            if (auto closed = input_.hands->Close(); closed.HasError())
                return closed;
            input_.hands.reset();
        }
        input_.handAvailability = XRCapabilityState::Unsupported;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::SampleHands */
    Result<OpenXRApplicationSession::HandInputSample> OpenXRApplicationSession::SampleHands(const XRSessionId &session,
                                                                                            const XRSessionLifecycle &sessions,
                                                                                            const XRTimeEvidence &time) {
        using Outcome = Result<HandInputSample>;
        if (!input_.hands || input_.handSamplingBlocked)
            return Outcome::Success({{}, input_.handSamplingBlocked.value_or(input_.handAvailability)});
        const auto space = WorldSpace(session);
        if (space.HasError())
            return Outcome::Failure(space.ErrorValue());
        const auto reference = BorrowWorldSpace(space.Value());
        if (reference.HasError())
            return Outcome::Failure(reference.ErrorValue());
        auto tracked = input_.hands->Sample(reference.Value(), space.Value(), time);
        if (tracked.HasError()) {
            const auto outage = HandOutage(tracked.ErrorValue());
            const auto plan = sessions.AcceptedPlan();
            if (!outage || !plan || plan->Decision(XRCapability::HandTracking).state != XRFeatureDecisionState::OptionalEnabled)
                return Outcome::Failure(tracked.ErrorValue());
            if (*outage != XRCapabilityState::Incompatible)
                input_.handSamplingBlocked = outage;
            return Outcome::Success({{}, *outage});
        }
        const auto &hands = tracked.Value().hands;
        if (std::ranges::none_of(hands, [](const auto &hand) {
            return hand.joints[static_cast<std::size_t>(XRHandJoint::Palm)].pose.has_value();
        }))
            return Outcome::Success({{}, XRCapabilityState::TemporarilyUnavailable});
        return Outcome::Success({std::move(tracked).Value(), XRCapabilityState::Available});
    }

    /** @copydoc OpenXRApplicationSession::PrepareInput */
    Result<void> OpenXRApplicationSession::PrepareInput(const Input::InputActionCollection &port, const Input::InputRouter &router,
                                                        const OpenXRApplicationBindings &bindings,
                                                        const NativeTrackingSpaceRequest &tracking, const bool gazeConsent) {
        // Retain the caller's original tracking plan before admission and loader callbacks can reenter foreign code.
        NativeTrackingSpaceRequest admittedTracking = tracking;
        if (auto current = Current(); current.HasError())
            return current;
        if (input_.native || input_.projection || input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto census = PrepareSession();
        if (census.HasError())
            return Result<void>::Failure(census.ErrorValue());
        const auto spaces = std::span{census.Value().spaces}.first(census.Value().spaceCount);
        const auto hasReference = [&](XrReferenceSpaceType type) {
            return std::ranges::find(spaces, type) != spaces.end();
        };
        if (!hasReference(XR_REFERENCE_SPACE_TYPE_VIEW) || !hasReference(XR_REFERENCE_SPACE_TYPE_LOCAL))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto features = settings_.Features();
        const auto stage = std::ranges::find(features, XRCapability::StageSpace, &XRFeatureRequest::capability);
        const bool stageRequired =
            admittedTracking.base == XRSpaceKind::Stage || (stage != features.end() && stage->kind == XRFeatureRequirementKind::Required);
        const bool wantsStage = stageRequired || (stage != features.end() && stage->kind == XRFeatureRequirementKind::Optional);
        // The two eyes reserve space IDs from this same budget, even though they create no native XrSpace.
        constexpr std::size_t baselineSpaces = 5;  // Local, View, calibrated World, and two eye identities.
        const auto minimumSpaces = baselineSpaces + bindings.poses.size();
        const auto capacity = settings_.Limits().maximumSpaces;
        if (capacity < minimumSpaces)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const bool stageObserved = hasReference(XR_REFERENCE_SPACE_TYPE_STAGE);
        if (stageRequired && !stageObserved)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (stageRequired && capacity == minimumSpaces)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const bool stageAvailable = stageObserved && capacity > minimumSpaces;
        input_.stageAvailability = stageAvailable ? XRCapabilityState::Available : XRCapabilityState::Unavailable;
        admittedTracking.alternateReferenceAvailable = wantsStage && stageAvailable;
        admittedTracking.stageBoundaryWarningMeters =
            wantsStage && stageAvailable ? std::optional{settings_.StageBoundaryWarningMeters()} : std::nullopt;
        const auto trackingSpaces = minimumSpaces + static_cast<std::size_t>(admittedTracking.alternateReferenceAvailable);
        if (gazeConsent && MayArmEyeGaze() && capacity > trackingSpaces && !bindings.setLabels.empty() &&
            bindings.PreparedActionCount() < settings_.Limits().maximumActions)
            admittedTracking.consentedGazeSet = bindings.setLabels.front().set;
        else
            admittedTracking.consentedGazeSet.reset();
        input_.gazeAllowed = admittedTracking.consentedGazeSet.has_value();
        const auto discovery = native_->BorrowDiscovery(selection_.attempt);
        if (discovery.HasError())
            return Result<void>::Failure(discovery.ErrorValue());
        input_.native = std::make_unique<OpenXRActionBindings>(*native_);
        auto plan = bindings.Prepare(*input_.native, {*preflight_, selection_.attempt}, {discovery.Value().getProc}, router.Actions(),
                                     admittedTracking);
        if (plan.HasError())
            return Result<void>::Failure(plan.ErrorValue());
        const auto profiles = input_.native->PreparedProfiles({*preflight_, selection_.attempt});
        if (profiles.HasError())
            return Result<void>::Failure(profiles.ErrorValue());
        auto reserved = XRInputProjection::Prepare(port, plan.Value());
        if (reserved.HasError())
            return Result<void>::Failure(reserved.ErrorValue());
        input_.projection.emplace(std::move(reserved).Value());
        input_.bindings.emplace(std::move(plan).Value());
        input_.profiles = profiles.Value();
        if (auto hands = PrepareHands(); hands.HasError())
            return hands;
        return Current();
    }

    /** @copydoc OpenXRApplicationSession::BindInput */
    Result<void> OpenXRApplicationSession::BindInput(const XRSessionId &candidate, XRActionBindingCoordinator &coordinator,
                                                     const Input::InputRouter &router, const OpenXRApplicationBindings &bindings) {
        if (auto current = Current(); current.HasError())
            return current;
        if (!input_.native || !input_.projection || !input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto bound = input_.native->Bind(candidate); bound.HasError())
            return bound;
        const auto profiles = input_.native->Profiles(candidate);
        if (profiles.HasError())
            return Result<void>::Failure(profiles.ErrorValue());
        if (profiles.Value() != input_.profiles)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        if (auto published = bindings.Publish(coordinator, candidate, router.Actions(), profiles.Value()); published.HasError())
            return published;
        if (!coordinator.Current() || *coordinator.Current() != *input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        if (auto bound = input_.projection->Bind(candidate, coordinator.Revision()); bound.HasError())
            return bound;
        if (const auto native = native_->Borrow(candidate); native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        if (input_.hands)
            if (const auto hands = input_.hands->Bind(candidate); hands.HasError())
                return hands;
        // Actual dispatch is sealed cold on the same installed instance; frame sampling never resolves symbols.
        return input_.native->PreparePresentation({getProc_});
    }

    /** @copydoc OpenXRApplicationSession::RefreshInputProfiles */
    Result<void> OpenXRApplicationSession::RefreshInputProfiles(const XRSessionId &session) {
        if (auto current = Current(); current.HasError())
            return current;
        if (!input_.native || !input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto observed = input_.native->Profiles(session);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (const auto valid = input_.native->ValidateProfileContinuity(input_.profiles, observed.Value()); valid.HasError())
            return valid;
        input_.profiles = observed.Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::RequestHapticCancellation */
    void OpenXRApplicationSession::RequestHapticCancellation(const std::optional<XRTrackedDeviceRole> role) noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR haptic revocation requires its owner lane");
        if (input_.native)
            input_.native->RequestHapticCancellation(role);
    }

    /** @copydoc OpenXRApplicationSession::ProgressHapticCancellation */
    Result<void> OpenXRApplicationSession::ProgressHapticCancellation(const XRSessionId &session) {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return input_.native ? input_.native->ProgressHapticCancellation(session) : Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::SubmitHapticPreview */
    Result<void> OpenXRApplicationSession::SubmitHapticPreview(const XRSessionId &session, XRTrackedDeviceRole role, float amplitude,
                                                               Duration duration) {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!input_.native)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        return input_.native->ApplyHaptic(session, role, {.amplitude = amplitude, .duration = duration});
    }

    /** @copydoc OpenXRApplicationSession::AppendInput */
    Result<void> OpenXRApplicationSession::AppendInput(const XRSessionLifecycle &sessions, const XRActionBindingCoordinator &coordinator,
                                                       RuntimeInternal::XRTrackingPublication &publication, const OpenXRInputClock &clock,
                                                       const Runtime::FrameContext &context, const XRWorldOriginRevision origin,
                                                       const Input::InputCollectionFrame &frame, const Math::Transform *worldPlacement) {
        // Revoke prior evidence before foreign calls; a failed cutoff must not revive it.
        publication.Revoke();
        input_.stageBoundary.reset();
        input_.gaze.reset();
        input_.gazeRevision = {};
        if (auto current = Current(); current.HasError())
            return current;
        const auto state = sessions.Snapshot();
        if (!state.admitsActions)
            return Result<void>::Success();
        if (!native_ || native_->Session() != state.session || !input_.native || !input_.projection || !input_.bindings ||
            !coordinator.Current() || *coordinator.Current() != *input_.bindings || coordinator.Revision() == 0)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto bindingRevision = coordinator.Revision();
        const auto bindingsCurrent = [&] {
            return input_.native && input_.projection && input_.bindings && coordinator.Revision() == bindingRevision &&
                   coordinator.Current() && *coordinator.Current() == *input_.bindings;
        };
        const auto time = clock.Read(context);
        if (time.HasError())
            return Result<void>::Failure(time.ErrorValue());
        if (auto synced = input_.native->Sync(state.session); synced.HasError())
            return synced;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto poses = input_.native->ObserveWorldPoses(state.session, time.Value(), origin);
        if (poses.HasError())
            return Result<void>::Failure(poses.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto handSample = SampleHands(state.session, sessions, time.Value());
        if (handSample.HasError())
            return Result<void>::Failure(handSample.ErrorValue());
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto &hands = handSample.Value().hands;
        std::array<RuntimeInternal::TrackingSourceEvidence, 7> sources{};
        std::size_t sourceCount = 0;
        for (const auto &pose : poses.Value().Poses())
            sources[sourceCount++] = {pose.role, pose.kind, pose.active, pose.pose};
        if (hands)
            for (std::size_t hand = 0; hand < hands->hands.size(); ++hand) {
                const auto &palm = hands->hands[hand].joints[static_cast<std::size_t>(XRHandJoint::Palm)].pose;
                if (!palm)
                    continue;
                sources[sourceCount++] = {HandRole(hand), XRTrackedPoseKind::Palm, true, Describe(*palm)};
            }
        if (auto observed = publication.ObserveSources(std::span{sources}.first(sourceCount)); observed.HasError())
            return observed;
        bool controllerLost = false;
        for (const auto &device : publication.Devices()) {
            if (device.state == XRTrackedDeviceState::Tracked ||
                (device.role != XRTrackedDeviceRole::LeftController && device.role != XRTrackedDeviceRole::RightController))
                continue;
            RequestHapticCancellation(device.role);
            controllerLost = true;
        }
        if (controllerLost) {
            if (auto stopped = ProgressHapticCancellation(state.session); stopped.HasError())
                return stopped;
            if (!bindingsCurrent())
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        }
        const auto actions = input_.native->Sample(state.session, *coordinator.Current(), publication.Devices(), input_.profiles);
        if (actions.HasError())
            return Result<void>::Failure(actions.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        std::optional<XRPoseSample> gaze;
        if (input_.gazeAllowed) {
            auto observed = input_.native->SampleConsentedGaze(state.session, time.Value(), MaximumEyeGazeAge, input_.gazeAllowed);
            if (auto current = Current(); current.HasError())
                return current;
            if (!bindingsCurrent())
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
            if (observed.HasValue() && input_.gazeAllowed)
                gaze = std::move(observed).Value();
            else if (observed.HasError() &&
                     ErrorChainContains(observed.ErrorValue(), XRErrors::PermissionDenied.domain, XRErrors::PermissionDenied.code))
                RevokeEyeGaze();
        }
        if (gaze && worldPlacement) {
            auto placed = PlaceXRWorldPose(Describe(*gaze), *worldPlacement);
            if (placed.HasValue())
                gaze = std::move(placed).Value();
            else
                gaze.reset();
        }
        const auto appended = AppendPreparedInput({poses.Value(), actions.Value(), state, time.Value(), origin, bindingRevision,
                                                   handSample.Value(), worldPlacement},
                                                  publication, frame);
        if (appended.HasValue()) {
            input_.profiles = actions.Value().Profiles();
            if (input_.gazeAllowed)
                input_.gaze = std::move(gaze);
            if (const auto *tracking = publication.Current()) {
                if (const auto boundary = poses.Value().StageBoundary(); boundary && tracking->State() == XRTrackingState::Active)
                    input_.stageBoundary = StageBoundaryObservation{DescribeXRInteractionCutoff(*tracking), *boundary};
                AdvancePlaneCapture(*tracking);
                AdvanceSpatialAnchor(*tracking);
            }
            if (const auto *published = publication.Current(); published && input_.gaze)
                input_.gazeRevision = published->Revision();
        }
        return appended;
    }

    /** @copydoc OpenXRApplicationSession::ReadStageBoundary */
    std::optional<XRStageBoundaryProximity> OpenXRApplicationSession::ReadStageBoundary(const XRTrackingSnapshot &snapshot) const noexcept {
        if (std::this_thread::get_id() != lane_ || !input_.stageBoundary ||
            input_.stageBoundary->cutoff != DescribeXRInteractionCutoff(snapshot))
            return std::nullopt;
        return input_.stageBoundary->proximity;
    }

    /** @copydoc OpenXRApplicationSession::AppendPreparedInput */
    Result<void> OpenXRApplicationSession::AppendPreparedInput(const PreparedInputSample &sample,
                                                               XR::RuntimeInternal::XRTrackingPublication &publication,
                                                               const Input::InputCollectionFrame &frame) {
        const auto &[poses, actions, state, time, origin, bindingRevision, handSample, worldPlacement] = sample;
        const auto &hands = handSample.hands;
        std::array<RuntimeInternal::TrackingPoseEvidence, 7> normalized{};
        std::size_t poseCount = 0;
        bool tracked = false;
        for (const auto &pose : poses.Poses()) {
            const auto devices = publication.Devices();
            const auto device = std::ranges::find(devices, pose.role, &XRTrackedDeviceRecord::role);
            if (const auto capability = NativePoseCapability(pose.kind);
                device == devices.end() || device->state != XRTrackedDeviceState::Tracked || !device->capabilities.Has(capability))
                continue;
            auto descriptor = pose.pose;
            if (worldPlacement) {
                const auto placed = PlaceXRWorldPose(descriptor, *worldPlacement);
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                descriptor.components = placed.Value().Components();
            }
            normalized[poseCount++] = {device->id, pose.kind, descriptor};
            tracked |= descriptor.components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                       descriptor.components.orientation.validity != XRPoseComponentValidity::Invalid;
        }
        std::array<XRHandJointRecord, XRHandTrackingHardLimits::MaximumJoints> joints;
        std::size_t jointCount = 0;
        if (hands) {
            const auto devices = publication.Devices();
            for (std::size_t handIndex = 0; handIndex < hands->hands.size(); ++handIndex) {
                const auto device = std::ranges::find(devices, HandRole(handIndex), &XRTrackedDeviceRecord::role);
                if (device == devices.end() || device->state != XRTrackedDeviceState::Tracked)
                    continue;
                const auto &hand = hands->hands[handIndex];
                for (std::size_t index = 0; index < hand.joints.size(); ++index) {
                    const auto &joint = hand.joints[index];
                    if (!joint.pose)
                        continue;
                    auto pose = worldPlacement ? PlaceXRWorldPose(Describe(*joint.pose), *worldPlacement)
                                               : Result<XRPoseSample>::Success(*joint.pose);
                    if (pose.HasError())
                        return Result<void>::Failure(pose.ErrorValue());
                    const auto kind = static_cast<XRHandJoint>(index);
                    if (kind == XRHandJoint::Palm)
                        normalized[poseCount++] = {device->id, XRTrackedPoseKind::Palm, Describe(pose.Value())};
                    joints[jointCount++] = {device->id, static_cast<XRHandedness>(handIndex), kind, std::move(pose).Value(),
                                            joint.radiusMeters};
                }
            }
            tracked |= jointCount != 0;
        }
        if (const auto candidate = publication.Publish(
                {time, origin, XRTrackingFocusState::Focused,
                 state.state == XRSessionState::Focused || state.state == XRSessionState::Visible ? XRTrackingVisibilityState::Visible
                                                                                                  : XRTrackingVisibilityState::Hidden,
                 tracked ? XRTrackingState::Active : XRTrackingState::TrackingLost, bindingRevision, std::span{normalized}.first(poseCount),
                 actions.Actions(), std::span{joints}.first(jointCount)});
            candidate.HasError())
            return candidate;
        const auto *snapshot = publication.Current();
        if (!snapshot)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto revision = snapshot->Revision();
        XRHandGestureCandidates gestures;
        if (hands) {
            const auto &policy = authorities_.handGestures ? *authorities_.handGestures : pinchPolicy_;
            auto derived = DeriveXRHandGestures(*snapshot, state.session, revision, origin, policy);
            if (derived.HasError()) {
                publication.Revoke();
                return Result<void>::Failure(derived.ErrorValue());
            }
            gestures = std::move(derived).Value();
            snapshot = publication.Current();
            if (!snapshot || snapshot->Revision() != revision || !input_.projection) {
                publication.Revoke();
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
            }
        }
        const auto appended = input_.projection->Append(*snapshot, state.session, revision, origin, bindingRevision, frame);
        if (appended.HasError())
            publication.Revoke();
        else
            input_.handCutoff.emplace(HandCutoffObservation{revision, handSample.availability, std::move(gestures)});
        return appended;
    }

    /** @copydoc OpenXRApplicationSession::CurrentHandCutoff */
    const OpenXRApplicationSession::HandCutoffObservation *OpenXRApplicationSession::CurrentHandCutoff(
        const XRTrackingSnapshot &snapshot) const noexcept {
        if (std::this_thread::get_id() != lane_ || !native_ || native_->Session() != snapshot.Session() || !input_.handCutoff ||
            input_.handCutoff->revision != snapshot.Revision())
            return nullptr;
        return &*input_.handCutoff;
    }

    /** @copydoc OpenXRApplicationSession::ReadHandGestures */
    XRHandGestureCandidates OpenXRApplicationSession::ReadHandGestures(const XRTrackingSnapshot &snapshot) const noexcept {
        const auto *current = CurrentHandCutoff(snapshot);
        return current ? current->gestures : XRHandGestureCandidates{};
    }

    /** @copydoc OpenXRApplicationSession::ReadHandAvailability */
    std::optional<XRCapabilityState> OpenXRApplicationSession::ReadHandAvailability(const XRTrackingSnapshot &snapshot) const noexcept {
        if (const auto *current = CurrentHandCutoff(snapshot))
            return current->availability;
        return std::nullopt;
    }

    /** @copydoc OpenXRApplicationSession::PreparedBindings */
    const XRResolvedActionBindings *OpenXRApplicationSession::PreparedBindings() const noexcept {
        return std::this_thread::get_id() == lane_ && input_.bindings ? &*input_.bindings : nullptr;
    }

    /** @copydoc OpenXRApplicationSession::References */
    Result<XRReferenceSpaceSnapshot> OpenXRApplicationSession::References(const XRSessionId &candidate) const {
        if (auto current = Current(); current.HasError())
            return Result<XRReferenceSpaceSnapshot>::Failure(current.ErrorValue());
        if (!input_.native)
            return Result<XRReferenceSpaceSnapshot>::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->References(candidate);
    }

    /** @copydoc OpenXRApplicationSession::WorldSpace */
    Result<XRCoordinateSpace> OpenXRApplicationSession::WorldSpace(const XRSessionId &candidate) const {
        if (auto current = Current(); current.HasError())
            return Result<XRCoordinateSpace>::Failure(current.ErrorValue());
        if (!input_.native)
            return Result<XRCoordinateSpace>::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->WorldSpace(candidate);
    }

    /** @copydoc OpenXRApplicationSession::ObserveReferenceCalibration */
    Result<NativeReferenceCalibrationObservation> OpenXRApplicationSession::ObserveReferenceCalibration(const XRCoordinateSpace reference,
                                                                                                        const XRCoordinateSpace world,
                                                                                                        const XRRuntimeTime now) const {
        using Outcome = Result<NativeReferenceCalibrationObservation>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!input_.native)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->ObserveReferenceCalibration(reference, world, now);
    }

    /** @copydoc OpenXRApplicationSession::BorrowWorldSpace */
    Result<XrSpace> OpenXRApplicationSession::BorrowWorldSpace(const XRCoordinateSpace &space) const {
        if (auto current = Current(); current.HasError())
            return Result<XrSpace>::Failure(current.ErrorValue());
        if (!input_.native)
            return Result<XrSpace>::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->BorrowWorldSpace(space);
    }

    /** @copydoc OpenXRApplicationSession::BorrowPresentationReference */
    Result<XrSpace> OpenXRApplicationSession::BorrowPresentationReference(const XRCoordinateSpace &space) const {
        if (auto current = Current(); current.HasError())
            return Result<XrSpace>::Failure(current.ErrorValue());
        if (!input_.native)
            return Result<XrSpace>::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->BorrowPresentationReference(space);
    }

    /** @copydoc OpenXRApplicationSession::LocatePresentation */
    Result<NativePresentationViews> OpenXRApplicationSession::LocatePresentation(const XRSessionId &candidate,
                                                                                 const XRTimeEvidence &time) const {
        if (auto current = Current(); current.HasError())
            return Result<NativePresentationViews>::Failure(current.ErrorValue());
        if (!input_.native || !system_)
            return Result<NativePresentationViews>::Failure(MakeError(XRErrors::OperationUnavailable));
        return input_.native->LocatePresentation(candidate, time, system_->primaryViewCount);
    }

    /** @copydoc OpenXRApplicationSession::MayArmPlaneCapture */
    bool OpenXRApplicationSession::MayArmPlaneCapture() const noexcept {
        return settings_.AllowsPlaneCapturePreview() && system_ && system_->planes.state == XRCapabilityState::Available;
    }

    /** @copydoc OpenXRApplicationSession::SetPlaneCaptureConsent */
    void OpenXRApplicationSession::SetPlaneCaptureConsent(const bool allowed) noexcept {
        if (!allowed || !MayArmPlaneCapture()) {
            RevokePlaneCapture();
            return;
        }
        if (!planes_.allowed && planes_.lastGrant != std::numeric_limits<std::uint64_t>::max()) {
            ++planes_.lastGrant;
            planes_.allowed = true;
        }
    }

    /** @copydoc OpenXRApplicationSession::RevokePlaneCapture */
    void OpenXRApplicationSession::RevokePlaneCapture() noexcept {
        planes_.allowed = false;
        planes_.scope.reset();
        planes_.latestCutoff = {};
        planes_.publishedAt = {};
        if (planes_.native)
            planes_.native->Revoke();
    }

    /** @copydoc OpenXRApplicationSession::RetirePlaneCapture */
    Result<void> OpenXRApplicationSession::RetirePlaneCapture() {
        RevokePlaneCapture();
        if (planes_.native) {
            if (auto retired = planes_.native->Close(); retired.HasError())
                return retired;
            planes_.native.reset();
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::ProgressPlaneRetirement */
    Result<void> OpenXRApplicationSession::ProgressPlaneRetirement() {
        if (!planes_.allowed && planes_.native)
            return RetirePlaneCapture();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::PlaneCaptureAvailability */
    XRCapabilityState OpenXRApplicationSession::PlaneCaptureAvailability() const noexcept {
        if (!planes_.allowed)
            return XRCapabilityState::Unavailable;
        if (planes_.publishedAt.IsValid() && planes_.latestCutoff >= planes_.publishedAt &&
            planes_.latestCutoff.Nanoseconds() - planes_.publishedAt.Nanoseconds() <= PlaneCapturePolicy.maximumAge.ToNanoseconds())
            return XRCapabilityState::Available;
        return XRCapabilityState::TemporarilyUnavailable;
    }

    /** @copydoc OpenXRApplicationSession::ReadPlaneObservations */
    Result<XRPlaneSnapshot> OpenXRApplicationSession::ReadPlaneObservations(const XRTrackingSnapshot &tracking) const {
        using Outcome = Result<XRPlaneSnapshot>;
        if (std::this_thread::get_id() != lane_ || !planes_.allowed || !planes_.scope || !planes_.native)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (tracking.Focus() != XRTrackingFocusState::Focused || tracking.State() != XRTrackingState::Active ||
            tracking.Session() != planes_.scope->session || tracking.SampledAt() != planes_.latestCutoff)
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto scope = *planes_.scope;
        const auto now = planes_.latestCutoff;
        auto result = planes_.native->Read(scope, now);
        // Revocation clears the exact scope before retiring the native capture.
        if (planes_.scope != scope)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        return result;
    }

    /** @copydoc OpenXRApplicationSession::AdvancePlaneCapture */
    void OpenXRApplicationSession::AdvancePlaneCapture(const XRTrackingSnapshot &tracking) {
        if (!planes_.allowed)
            return;
        if (tracking.Focus() != XRTrackingFocusState::Focused || tracking.State() != XRTrackingState::Active) {
            if (planes_.native)
                RevokePlaneCapture();
            return;
        }
        const auto session = tracking.Session();
        const auto now = tracking.SampledAt();
        const auto grant = planes_.lastGrant;
        const auto admitted = planes_.scope;
        const auto currentGrant = [&] {
            return planes_.allowed && planes_.lastGrant == grant && planes_.scope == admitted;
        };
        const auto space = WorldSpace(session);
        if (!currentGrant())
            return;
        if (space.HasError()) {
            RevokePlaneCapture();
            return;
        }
        const auto world = BorrowWorldSpace(space.Value());
        // Host admission can revoke or replace the grant during either lookup.
        if (!currentGrant())
            return;
        if (world.HasError()) {
            RevokePlaneCapture();
            return;
        }
        if (!planes_.native) {
            if (!StartPlaneCapture(session, space.Value(), world.Value(), now) && currentGrant())
                RevokePlaneCapture();
            return;
        }
        if (!planes_.scope) {
            RevokePlaneCapture();
            return;
        }
        const XRPlaneScope scope{session, space.Value(), planes_.scope->grant};
        const auto progress = planes_.native->Advance(scope, world.Value(), now);
        if (!currentGrant())
            return;
        if (progress.HasError() || planes_.scope != scope) {
            RevokePlaneCapture();
            return;
        }
        planes_.latestCutoff = now;
        if (progress.Value())
            planes_.publishedAt = now;
    }

    /** @copydoc OpenXRApplicationSession::StartPlaneCapture */
    bool OpenXRApplicationSession::StartPlaneCapture(const XRSessionId &session, const XRCoordinateSpace &space, XrSpace world,
                                                     XRRuntimeTime now) {
        const auto consent = planes_.lastGrant;
        const auto grant = XRPlaneGrant::Create(consent);
        if (grant.HasError())
            return false;
        planes_.native.emplace(*native_);
        const XRPlaneScope scope{session, space, grant.Value()};
        if (const auto started = planes_.native->Start(selection_.attempt, scope, world, PlaneCapturePolicy, now);
            started.HasError() || !planes_.allowed || planes_.lastGrant != consent)
            return false;
        planes_.scope = scope;
        planes_.latestCutoff = now;
        return true;
    }

    /** @copydoc OpenXRApplicationSession::RevokeEyeGaze */
    void OpenXRApplicationSession::RevokeEyeGaze() noexcept {
        input_.gazeAllowed = false;
        input_.gaze.reset();
        input_.gazeRevision = {};
    }

    /** @copydoc OpenXRApplicationSession::ReadEyeGaze */
    std::optional<XRPoseSample> OpenXRApplicationSession::ReadEyeGaze(const XRTrackingSnapshot &snapshot, const XRRuntimeTime now) const noexcept {
        if (std::this_thread::get_id() != lane_ || !input_.gazeAllowed || !input_.gaze || input_.gazeRevision != snapshot.Revision() ||
            input_.gaze->Session() != snapshot.Session() || snapshot.Focus() != XRTrackingFocusState::Focused ||
            snapshot.State() != XRTrackingState::Active)
            return std::nullopt;
        const auto observed = input_.gaze->Time().runtimeSample;
        if (now < observed || now.Nanoseconds() - observed.Nanoseconds() > MaximumEyeGazeAge.ToNanoseconds())
            return std::nullopt;
        return input_.gaze;
    }

    /** @copydoc OpenXRApplicationSession::RevokeInput */
    void OpenXRApplicationSession::RevokeInput() noexcept {
        RetireInputProjection();
        input_.bindings.reset();
        input_.profiles = {};
    }

    /** @copydoc OpenXRApplicationSession::RetireInputProjection */
    void OpenXRApplicationSession::RetireInputProjection() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Input retirement requires its owner lane");
        input_.stageBoundary.reset();
        RequestHapticCancellation();
        RevokeEyeGaze();
        RevokePlaneCapture();
        RevokeSpatialAnchor();
        input_.projection.reset();
        input_.handCutoff.reset();
    }

    /** @copydoc OpenXRApplicationSession::RebuildInputProjection */
    Result<void> OpenXRApplicationSession::RebuildInputProjection(const Input::InputActionCollection &port,
                                                                  const XRActionBindingCoordinator &coordinator,
                                                                  const OpenXRApplicationBindings &bindings,
                                                                  const std::span<const Input::ActionDescriptor> registeredActions) {
        if (auto current = Current(); current.HasError())
            return current;
        if (!native_ || native_->Session() != coordinator.Session() || !input_.native || input_.projection || !input_.bindings ||
            !coordinator.Current() || *coordinator.Current() != *input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        if (const auto valid = bindings.ValidateRegisteredActions(registeredActions); valid.HasError())
            return valid;
        auto source = XRInputProjection::Create(port, coordinator.Session(), *coordinator.Current(), coordinator.Revision());
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        input_.projection.emplace(std::move(source).Value());
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::HasInputProjection */
    bool OpenXRApplicationSession::HasInputProjection() const noexcept {
        return input_.projection.has_value();
    }
}  // namespace Horo::Application::Internal
