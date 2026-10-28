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
    }  // namespace

    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRApplicationSession::PrepareInput */
    Result<void> OpenXRApplicationSession::PrepareInput(const Input::InputActionCollection &port, const Input::InputRouter &router,
                                                        const OpenXRApplicationBindings &bindings,
                                                        const NativeTrackingSpaceRequest &tracking) {
        // Retain the caller's original tracking plan before admission and loader callbacks can reenter foreign code.
        const NativeTrackingSpaceRequest admittedTracking = tracking;
        if (auto current = Current(); current.HasError())
            return current;
        if (input_.native || input_.projection || input_.bindings)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto census = PrepareSession(); census.HasError())
            return Result<void>::Failure(census.ErrorValue());
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
        // Actual dispatch is sealed cold on the same installed instance; frame sampling never resolves symbols.
        return input_.native->PreparePresentation({getProc_});
    }

    /** @copydoc OpenXRApplicationSession::AppendInput */
    Result<void> OpenXRApplicationSession::AppendInput(const XRSessionLifecycle &sessions, const XRActionBindingCoordinator &coordinator,
                                                       RuntimeInternal::XRTrackingPublication &publication, const OpenXRInputClock &clock,
                                                       const Runtime::FrameContext &context, const XRWorldOriginRevision origin,
                                                       Input::InputCollectionFrame &frame) {
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
        std::array<RuntimeInternal::TrackingSourceEvidence, 5> sources{};
        std::size_t sourceCount = 0;
        for (const auto &pose : poses.Value().Poses())
            sources[sourceCount++] = {pose.role, pose.kind, pose.active, pose.pose};
        if (auto observed = publication.ObserveSources(std::span{sources}.first(sourceCount)); observed.HasError())
            return observed;
        const auto actions = input_.native->Sample(state.session, *coordinator.Current(), publication.Devices());
        if (actions.HasError())
            return Result<void>::Failure(actions.ErrorValue());
        if (auto current = Current(); current.HasError())
            return current;
        if (!bindingsCurrent())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return AppendPreparedInput({poses.Value(), actions.Value(), state, time.Value(), origin, bindingRevision}, publication, frame);
    }

    /** @copydoc OpenXRApplicationSession::AppendPreparedInput */
    Result<void> OpenXRApplicationSession::AppendPreparedInput(const PreparedInputSample &sample,
                                                               XR::RuntimeInternal::XRTrackingPublication &publication,
                                                               Input::InputCollectionFrame &frame) {
        const auto &[poses, actions, state, time, origin, bindingRevision] = sample;
        std::array<RuntimeInternal::TrackingPoseEvidence, 5> normalized{};
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
        if (const auto candidate = publication.Publish({time, origin, XRTrackingFocusState::Focused,
                                                        state.state == XRSessionState::Focused || state.state == XRSessionState::Visible
                                                            ? XRTrackingVisibilityState::Visible
                                                            : XRTrackingVisibilityState::Hidden,
                                                        tracked ? XRTrackingState::Active : XRTrackingState::TrackingLost, bindingRevision,
                                                        std::span{normalized}.first(poseCount), actions.Actions()});
            candidate.HasError())
            return candidate;
        const auto *snapshot = publication.Current();
        if (!snapshot)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        const auto appended = input_.projection->Append(*snapshot, state.session, snapshot->Revision(), origin, bindingRevision, frame);
        if (appended.HasError())
            publication.Revoke();
        return appended;
    }

    /** @copydoc OpenXRApplicationSession::PreparedBindings */
    const XRResolvedActionBindings *OpenXRApplicationSession::PreparedBindings() const noexcept {
        return std::this_thread::get_id() == lane_ && input_.bindings ? &*input_.bindings : nullptr;
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
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Input retirement requires its owner lane");
        input_.projection.reset();
        input_.bindings.reset();
        input_.profiles = {};
    }
}  // namespace Horo::Application::Internal
