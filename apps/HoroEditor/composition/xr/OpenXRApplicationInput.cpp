#include "Horo/Foundation/Assertions.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRInputClock.h"
#include "XRTrackingPublication.h"

#include <algorithm>

namespace Horo::Application::Internal {
    namespace {
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
        if (input_.hands) {
            if (auto closed = input_.hands->Close(); closed.HasError())
                return closed;
            input_.hands.reset();
        }
        input_.handAvailability = XRCapabilityState::Unsupported;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::PrepareInput */
    Result<void> OpenXRApplicationSession::PrepareInput(const Input::InputActionCollection &port, const Input::InputRouter &router,
                                                        const OpenXRApplicationBindings &bindings,
                                                        const NativeTrackingSpaceRequest &tracking) {
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
    Result<void> OpenXRApplicationSession::RefreshInputProfiles(const XRSessionId &session, const XRInteractionProfileId fallback) {
        if (auto current = Current(); current.HasError())
            return current;
        if (!input_.native || !input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto observed = input_.native->Profiles(session);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        for (std::size_t index = 0; index < input_.profiles.size(); ++index) {
            const auto previous = input_.profiles[index].profile;
            const auto current = observed.Value()[index].profile;
            // Confirming the configured fallback resolves the same controls as the pre-running selection.
            if (current != previous && (previous.value != 0 || current != fallback))
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale, "OpenXR controller profile changed."));
        }
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

    /** @copydoc OpenXRApplicationSession::AppendInput */
    Result<void> OpenXRApplicationSession::AppendInput(const XRSessionLifecycle &sessions, const XRActionBindingCoordinator &coordinator,
                                                       RuntimeInternal::XRTrackingPublication &publication, const OpenXRInputClock &clock,
                                                       const Runtime::FrameContext &context, const XRWorldOriginRevision origin,
                                                       const Input::InputCollectionFrame &frame) {
        // Revoke the prior sample BEFORE any foreign call: failure cannot revive an earlier opportunity.
        publication.Revoke();
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
        const auto poses = input_.native->ObservePoses(state.session, time.Value(), origin);
        if (poses.HasError())
            return Result<void>::Failure(poses.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        std::optional<NativeHandSamples> hands;
        if (input_.hands) {
            const auto space = WorldSpace(state.session);
            if (space.HasError())
                return Result<void>::Failure(space.ErrorValue());
            const auto reference = BorrowWorldSpace(space.Value());
            if (reference.HasError())
                return Result<void>::Failure(reference.ErrorValue());
            auto tracked = input_.hands->Sample(reference.Value(), space.Value(), time.Value());
            if (tracked.HasError())
                return Result<void>::Failure(tracked.ErrorValue());
            if (!bindingsCurrent())
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
            hands.emplace(std::move(tracked).Value());
        }
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
        const auto actions = input_.native->Sample(state.session, *coordinator.Current(), publication.Devices());
        if (actions.HasError())
            return Result<void>::Failure(actions.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return AppendPreparedInput({poses.Value(), actions.Value(), state, time.Value(), origin, bindingRevision,
                                    hands ? &*hands : nullptr},
                                   publication, frame);
    }

    /** @copydoc OpenXRApplicationSession::AppendPreparedInput */
    Result<void> OpenXRApplicationSession::AppendPreparedInput(const PreparedInputSample &sample,
                                                               XR::RuntimeInternal::XRTrackingPublication &publication,
                                                               const Input::InputCollectionFrame &frame) {
        const auto &[poses, actions, state, time, origin, bindingRevision, hands] = sample;
        std::array<RuntimeInternal::TrackingPoseEvidence, 7> normalized{};
        std::size_t poseCount = 0;
        bool tracked = false;
        for (const auto &pose : poses.Poses()) {
            const auto devices = publication.Devices();
            const auto device = std::ranges::find(devices, pose.role, &XRTrackedDeviceRecord::role);
            if (const auto capability = NativePoseCapability(pose.kind);
                device == devices.end() || device->state == XRTrackedDeviceState::Disconnected || !device->capabilities.Has(capability))
                continue;
            normalized[poseCount++] = {device->id, pose.kind, pose.pose};
            tracked |= pose.pose.components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                       pose.pose.components.orientation.validity != XRPoseComponentValidity::Invalid;
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
                const auto &palm = hand.joints[static_cast<std::size_t>(XRHandJoint::Palm)].pose;
                if (palm)
                    normalized[poseCount++] = {device->id, XRTrackedPoseKind::Palm, Describe(*palm)};
                for (std::size_t index = 0; index < hand.joints.size(); ++index) {
                    const auto &joint = hand.joints[index];
                    if (joint.pose)
                        joints[jointCount++] = {device->id, static_cast<XRHandedness>(handIndex), static_cast<XRHandJoint>(index),
                                                joint.pose, joint.radiusMeters};
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
        if (hands) {
            const auto &policy = authorities_.handGestures ? *authorities_.handGestures : pinchPolicy_;
            auto gestures = DeriveXRHandGestures(*snapshot, state.session, snapshot->Revision(), origin, policy);
            if (gestures.HasError()) {
                publication.Revoke();
                return Result<void>::Failure(gestures.ErrorValue());
            }
            input_.gestures = std::move(gestures).Value();
            input_.gestureRevision = snapshot->Revision();
        }
        const auto appended = input_.projection->Append(*snapshot, state.session, snapshot->Revision(), origin, bindingRevision, frame);
        if (appended.HasError())
            publication.Revoke();
        return appended;
    }

    /** @copydoc OpenXRApplicationSession::ReadHandGestures */
    XRHandGestureCandidates OpenXRApplicationSession::ReadHandGestures(const XRTrackingSnapshot &snapshot) const noexcept {
        if (std::this_thread::get_id() != lane_ || !native_ || native_->Session() != snapshot.Session() ||
            input_.gestureRevision != snapshot.Revision())
            return {};
        return input_.gestures;
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

    /** @copydoc OpenXRApplicationSession::RevokeInput */
    void OpenXRApplicationSession::RevokeInput() noexcept {
        RetireInputProjection();
        input_.bindings.reset();
        input_.profiles = {};
    }

    /** @copydoc OpenXRApplicationSession::RetireInputProjection */
    void OpenXRApplicationSession::RetireInputProjection() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Input retirement requires its owner lane");
        RequestHapticCancellation();
        input_.projection.reset();
        input_.gestures = {};
        input_.gestureRevision = {};
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
