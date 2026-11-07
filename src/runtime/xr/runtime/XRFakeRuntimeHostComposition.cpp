#include "host/XRFakeRuntimeHostComposition.h"

namespace Horo::XR::RuntimeInternal {
    XRFakeRuntimeHostComposition::XRFakeRuntimeHostComposition(IXRSessionResources *children) noexcept : children_(children) {
        fake_.resources_.composition = this;
    }

    XRFakeRuntimeHostComposition::~XRFakeRuntimeHostComposition() {
        Shutdown();
        fake_.resources_.composition = nullptr;
    }

    Result<void> XRFakeRuntimeHostComposition::Lane() const {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (shutdown_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    Result<XRSessionId> XRFakeRuntimeHostComposition::Activate(const XRCapabilitySnapshot &capabilities, const XRFeaturePlan &plan,
                                                               const XRFrameLimits frames, const XRTrackingSnapshotLimits tracking,
                                                               const XRWorldOriginRevision origin,
                                                               const std::optional<XRSessionPreparation> failStage) {
        if (auto lane = Lane(); lane.HasError())
            return Result<XRSessionId>::Failure(lane.ErrorValue());
        Operation operation{busy_};
        nextLimits_ = tracking;
        nextOrigin_ = origin;
        auto result = fake_.Activate(capabilities, plan, frames, failStage);
        if (result.HasError())
            return result;
        HORO_INVARIANT(candidate_.session == result.Value() && candidate_.tracking);
        active_ = std::move(candidate_);
        candidate_ = {};
        sampledAt_ = {};
        return result;
    }

    Result<void> XRFakeRuntimeHostComposition::PrepareTracking(const XRSessionId &candidate, const XRFeaturePlan &plan) {
        if (nextLimits_.maximumDevices > plan.Limits().maximumDevices || nextLimits_.maximumActions > plan.Limits().maximumActions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (nextLimits_.maximumHandJoints != 0) {
            const auto decision = plan.Decision(XRCapability::HandTracking).state;
            if (decision != XRFeatureDecisionState::Required && decision != XRFeatureDecisionState::OptionalEnabled)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        }
        auto publication = std::make_unique<XRTrackingPublication>(fake_.sessions_);
        if (auto bound = publication->Bind(candidate, nextLimits_, nextOrigin_); bound.HasError())
            return bound;
        candidate_.tracking = std::move(publication);
        return Result<void>::Success();
    }

    Result<void> XRFakeRuntimeHostComposition::Prepare(const XRSessionPreparation stage, const XRSessionId &candidate,
                                                       const XRFeaturePlan &plan) {
        if (!candidate_.session.IsValid())
            candidate_.session = candidate;
        if (candidate_.session != candidate)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (stage == XRSessionPreparation::CapabilitiesAndSpaces)
            if (auto tracking = PrepareTracking(candidate, plan); tracking.HasError())
                return tracking;
        if (children_) {
            if (auto child = children_->Prepare(stage, candidate, plan); child.HasError())
                return child;
            candidate_.children[static_cast<std::size_t>(stage)] = true;
        }
        return Result<void>::Success();
    }

    XRFakeRuntimeHostComposition::State *XRFakeRuntimeHostComposition::Find(const XRSessionId &session) noexcept {
        if (candidate_.session == session)
            return &candidate_;
        return active_.session == session ? &active_ : nullptr;
    }

    void XRFakeRuntimeHostComposition::Release(const XRSessionPreparation stage, const XRSessionId &session) noexcept {
        auto *state = Find(session);
        if (!state)
            return;
        auto &prepared = state->children[static_cast<std::size_t>(stage)];
        if (children_ && prepared) {
            prepared = false;
            children_->Release(stage, session);
        }
        if (stage == XRSessionPreparation::CapabilitiesAndSpaces && state->tracking) {
            state->tracking->Close(session);
            state->tracking.reset();
        }
        if (stage == XRSessionPreparation::PlatformLoader)
            *state = {};
    }

    Result<void> XRFakeRuntimeHostComposition::AdmitSample(const XRSessionId &session) const {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (session != active_.session || !active_.tracking)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (!sampledAt_.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    Result<XRFakeOutcome> XRFakeRuntimeHostComposition::Step(const XRSessionId &session, const XRFakeStep &step) {
        if (auto lane = Lane(); lane.HasError())
            return Result<XRFakeOutcome>::Failure(lane.ErrorValue());
        Operation operation{busy_};
        auto outcome = fake_.Step(session, step);
        if (session == active_.session && active_.tracking) {
            active_.tracking->Revoke();
            sampledAt_ = outcome.status == XRFrameStatus::Ok ? outcome.actionSampleTime : XRRuntimeTime{};
        }
        return Result<XRFakeOutcome>::Success(std::move(outcome));
    }

    Result<void> XRFakeRuntimeHostComposition::ObserveSources(const XRSessionId &session,
                                                              const std::span<const TrackingSourceEvidence> sources) {
        if (auto admitted = AdmitSample(session); admitted.HasError())
            return admitted;
        return active_.tracking->ObserveSources(sources);
    }

    Result<void> XRFakeRuntimeHostComposition::Publish(const XRSessionId &session, const TrackingPublicationRequest &request) {
        if (auto admitted = AdmitSample(session); admitted.HasError())
            return admitted;
        if (request.time.runtimeSample != sampledAt_) {
            active_.tracking->Revoke();
            return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        }
        return active_.tracking->Publish(request);
    }

    Result<XRDeviceId> XRFakeRuntimeHostComposition::ObserveDevice(const XRSessionId &session, const XRTrackedDeviceRole role,
                                                                   const XRTrackedDeviceState state,
                                                                   const XRTrackedDeviceCapabilities capabilities) {
        if (auto admitted = AdmitSample(session); admitted.HasError())
            return Result<XRDeviceId>::Failure(admitted.ErrorValue());
        return active_.tracking->ObserveDevice(role, state, capabilities);
    }

    void XRFakeRuntimeHostComposition::RevokeTracking() noexcept {
        if (std::this_thread::get_id() == lane_ && !busy_ && active_.tracking)
            active_.tracking->Revoke();
    }

    std::span<const XRTrackedDeviceRecord> XRFakeRuntimeHostComposition::Devices() const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || !active_.tracking)
            return {};
        return active_.tracking->Devices();
    }

    const XRTrackingSnapshot *XRFakeRuntimeHostComposition::Current() const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || !active_.tracking)
            return nullptr;
        return active_.tracking->Current();
    }

    XRSessionSnapshot XRFakeRuntimeHostComposition::Snapshot() const noexcept {
        return std::this_thread::get_id() == lane_ ? fake_.Snapshot() : XRSessionSnapshot{};
    }

    void XRFakeRuntimeHostComposition::Shutdown() noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || shutdown_)
            return;
        Operation operation{busy_};
        sampledAt_ = {};
        fake_.Shutdown();
        shutdown_ = true;
    }
}  // namespace Horo::XR::RuntimeInternal
