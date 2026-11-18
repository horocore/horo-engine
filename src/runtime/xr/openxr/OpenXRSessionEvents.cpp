#include "OpenXRSessionEvents.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Revokes the owner-lane foreign-call reservation even when a test/native callback throws. */
        struct DispatchScope final {
            DispatchScope(const DispatchScope &) = delete;
            DispatchScope &operator=(const DispatchScope &) = delete;

            bool &active;

            explicit DispatchScope(bool &value) noexcept : active(value) {
                value = true;
            }

            ~DispatchScope() {
                active = false;
            }
        };

        /** @brief Loads one official ABI function without keeping a temporary or foreign resolver owner. */
        template <typename Function, typename Resolver>
        Result<void> Resolve(const Resolver &getProc, XrInstance instance, const char *name, Function &output) {
            PFN_xrVoidFunction raw{};
            if (const auto result = getProc(instance, name, &raw); result != XR_SUCCESS || !raw)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, std::format("{} unavailable (XrResult {})", name,
                                                                                                   static_cast<std::int32_t>(result))));
            output = std::bit_cast<Function>(raw);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRSessionEvents::OpenXRSessionEvents */
    OpenXRSessionEvents::OpenXRSessionEvents(OpenXRNativeSession &native, XRSessionLifecycle &sessions, XRFrameLifecycle &frames) noexcept
        : native_(&native), sessions_(&sessions), frames_(&frames) {}

    /** @copydoc OpenXRSessionEvents::Failure */
    Error OpenXRSessionEvents::Failure(const char *operation, const XrResult result) {
        return MakeError(XRErrors::OperationUnavailable,
                         std::format("{} failed (XrResult {})", operation, static_cast<std::int32_t>(result)));
    }

    /** @copydoc OpenXRSessionEvents::Current */
    Result<NativeSessionBorrow> OpenXRSessionEvents::Current() const {
        using Outcome = Result<NativeSessionBorrow>;
        if (std::this_thread::get_id() != lane_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        auto current = native_->Borrow(session_);
        if (current.HasError())
            return current;
        if (current.Value().incarnation != incarnation_)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return current;
    }

    /** @copydoc OpenXRSessionEvents::Bind */
    Result<void> OpenXRSessionEvents::Bind(NativeOpenXRDispatch dispatch, std::span<OpenXRSwapchain *const> swapchains) {
        const auto getProc = dispatch.getProc;
        if (std::this_thread::get_id() != lane_ || dispatching_ || session_.IsValid() || !getProc)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        DispatchScope scope{dispatching_};
        if (swapchains.size() > swapchains_.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        for (std::size_t index = 0; index < swapchains.size(); ++index)
            if (!swapchains[index] ||
                std::find(swapchains.begin(), swapchains.begin() + index, swapchains[index]) != swapchains.begin() + index)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const auto selected = sessions_->Snapshot().session;
        auto native = native_->Borrow(selected);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        Dispatch candidate;
        if (auto r = Resolve(getProc, native.Value().instance, "xrPollEvent", candidate.poll); r.HasError())
            return r;
        if (auto r = Resolve(getProc, native.Value().instance, "xrBeginSession", candidate.begin); r.HasError())
            return r;
        if (auto r = Resolve(getProc, native.Value().instance, "xrEndSession", candidate.end); r.HasError())
            return r;
        if (auto r = Resolve(getProc, native.Value().instance, "xrRequestExitSession", candidate.requestExit); r.HasError())
            return r;
        auto fenced = native_->Borrow(selected);
        if (fenced.HasError())
            return Result<void>::Failure(fenced.ErrorValue());
        if (fenced.Value().incarnation != native.Value().incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        dispatch_ = candidate;
        session_ = selected;
        incarnation_ = native.Value().incarnation;
        std::ranges::copy(swapchains, swapchains_.begin());
        swapchainCount_ = swapchains.size();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::ImagesRetired */
    bool OpenXRSessionEvents::ImagesRetired() const noexcept {
        if (frames_->Snapshot().phase != XRFramePhase::Idle)
            return false;
        for (std::size_t index = 0; index < swapchainCount_; ++index) {
            const auto phase = swapchains_[index]->Phase();
            if (phase != NativeSwapchainPhase::Empty && phase != NativeSwapchainPhase::Available)
                return false;
        }
        return true;
    }

    /** @copydoc OpenXRSessionEvents::StateChanged */
    Result<void> OpenXRSessionEvents::StateChanged(const XrEventDataSessionStateChanged &event, const XrSession_T *session) {
        if (event.session != session)
            return Result<void>::Success();
        if (event.state <= XR_SESSION_STATE_UNKNOWN || event.state > XR_SESSION_STATE_EXITING)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if ((sessions_->Snapshot().state == XRSessionState::Focused && event.state != XR_SESSION_STATE_FOCUSED) ||
            event.state == XR_SESSION_STATE_STOPPING || event.state == XR_SESSION_STATE_EXITING)
            native_->RevokePassthrough();
        observed_ = event.state;
        switch (event.state) {
            using enum Horo::XR::XRSessionEvent;
            case XR_SESSION_STATE_READY:
                return sessions_->ApplyEvent(session_, Ready);
            case XR_SESSION_STATE_VISIBLE:
                return sessions_->ApplyEvent(session_, sessions_->Snapshot().state == XRSessionState::Focused ? FocusLost : BecameVisible);
            case XR_SESSION_STATE_FOCUSED:
                return sessions_->ApplyEvent(session_, BecameFocused);
            case XR_SESSION_STATE_SYNCHRONIZED:
                if (sessions_->Snapshot().state == XRSessionState::Visible || sessions_->Snapshot().state == XRSessionState::Focused)
                    return sessions_->ApplyEvent(session_, VisibilityLost);
                return Result<void>::Success();
            case XR_SESSION_STATE_STOPPING:
                return sessions_->ApplyEvent(session_, Stopping);
            case XR_SESSION_STATE_LOSS_PENDING:
                return RecordLoss(NativeSessionEventLoss::Session);
            case XR_SESSION_STATE_EXITING:
                return running_ ? RecordLoss(NativeSessionEventLoss::EventHistory) : Result<void>::Success();
            default:
                return Result<void>::Success();
        }
    }

    /** @copydoc OpenXRSessionEvents::RecordLoss */
    Result<void> OpenXRSessionEvents::RecordLoss(const NativeSessionEventLoss reason) {
        native_->RevokePassthrough();
        const auto event = reason == NativeSessionEventLoss::Instance ? XRSessionEvent::InstanceLost : XRSessionEvent::SessionLost;
        if (auto applied = sessions_->ApplyEvent(session_, event); applied.HasError())
            return applied;
        loss_ = reason;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::CheckCall */
    Result<void> OpenXRSessionEvents::CheckCall(const char *operation, const XrResult result) {
        if (auto fence = Current(); fence.HasError())
            return Result<void>::Failure(fence.ErrorValue());
        switch (result) {
            case XR_SUCCESS:
                return Result<void>::Success();
            case XR_SESSION_LOSS_PENDING:
            case XR_ERROR_SESSION_LOST:
                return RecordLoss(NativeSessionEventLoss::Session);
            case XR_ERROR_INSTANCE_LOST:
                return RecordLoss(NativeSessionEventLoss::Instance);
            default:
                return Result<void>::Failure(Failure(operation, result));
        }
    }

    /** @copydoc OpenXRSessionEvents::Progress */
    Result<void> OpenXRSessionEvents::Progress(const bool wantsPlay) {
        const bool begin =
            !running_ && wantsPlay && observed_ == XR_SESSION_STATE_READY && sessions_->Snapshot().state == XRSessionState::Ready;
        const bool exit = running_ && !wantsPlay && !exitRequested_ && observed_ != XR_SESSION_STATE_STOPPING;
        const bool end = running_ && observed_ == XR_SESSION_STATE_STOPPING && ImagesRetired();
        if (!begin && !exit && !end)
            return Result<void>::Success();
        auto current = Current();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (begin) {
            const XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            const auto result = dispatch_.begin(current.Value().session, &info);
            // A native success still creates cleanup debt if a foreign call revoked the publication during begin.
            running_ = result == XR_SUCCESS;
            if (auto checked = CheckCall("xrBeginSession", result); checked.HasError() || loss_ != NativeSessionEventLoss::None)
                return checked;
            exitRequested_ = false;
            return sessions_->ApplyEvent(session_, XRSessionEvent::Started);
        }
        if (exit) {
            const auto result = dispatch_.requestExit(current.Value().session);
            exitRequested_ = result == XR_SUCCESS;
            if (auto checked = CheckCall("xrRequestExitSession", result); checked.HasError() || loss_ != NativeSessionEventLoss::None)
                return checked;
        }
        if (end) {
            const auto result = dispatch_.end(current.Value().session);
            if (result == XR_SUCCESS)
                running_ = false;
            if (auto checked = CheckCall("xrEndSession", result); checked.HasError() || loss_ != NativeSessionEventLoss::None)
                return checked;
            exitRequested_ = false;
            return sessions_->ApplyEvent(session_, XRSessionEvent::Idle);
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::ReadReferenceChange */
    Result<std::optional<XRReferenceSpaceChange>> OpenXRSessionEvents::ReadReferenceChange(
        const XrEventDataReferenceSpaceChangePending &event) const {
        using Outcome = Result<std::optional<XRReferenceSpaceChange>>;
        XRSpaceKind kind;
        switch (event.referenceSpaceType) {
            case XR_REFERENCE_SPACE_TYPE_VIEW:
                kind = XRSpaceKind::View;
                break;
            case XR_REFERENCE_SPACE_TYPE_LOCAL:
                kind = XRSpaceKind::Local;
                break;
            case XR_REFERENCE_SPACE_TYPE_STAGE:
                kind = XRSpaceKind::Stage;
                break;
            default:
                return Outcome::Success(std::nullopt);
        }
        const auto time = XRRuntimeTime::Create(event.changeTime);
        if (time.HasError() || (event.poseValid != XR_TRUE && event.poseValid != XR_FALSE))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Malformed OpenXR reference change evidence."));
        if (event.poseValid == XR_TRUE) {
            const auto &p = event.poseInPreviousSpace.position;
            const auto &q = event.poseInPreviousSpace.orientation;
            if (!Math::IsFinite(Math::Vec3{p.x, p.y, p.z}) || Math::Quaternion{q.x, q.y, q.z, q.w}.TryNormalized().HasError())
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Malformed OpenXR recenter pose."));
        }
        return Outcome::Success(XRReferenceSpaceChange{session_, kind, time.Value(), event.poseValid == XR_TRUE});
    }

    /** @copydoc OpenXRSessionEvents::ProcessEvent */
    Result<void> OpenXRSessionEvents::ProcessEvent(const XrEventDataBuffer &buffer, const XrSession_T *session,
                                                   NativeSessionEventObservation &observation) {
        if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged state;
            std::memcpy(&state, &buffer, sizeof(state));
            const bool admitted = sessions_->Snapshot().admitsActions;
            if (auto changed = StateChanged(state, session); changed.HasError())
                return Result<void>::Failure(changed.ErrorValue());
            observation.actionsInterrupted |= admitted && !sessions_->Snapshot().admitsActions;
            observation.exitRequested |= state.session == session && state.state == XR_SESSION_STATE_EXITING;
        } else if (buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            return RecordLoss(NativeSessionEventLoss::Instance);
        } else if (buffer.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            // Missing state transitions cannot authorize continued frames or a later Ready restart.
            // Logical loss retains native frame/image debt; it does not manufacture end/release success.
            return RecordLoss(NativeSessionEventLoss::EventHistory);
        } else if (buffer.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            XrEventDataInteractionProfileChanged event;
            std::memcpy(&event, &buffer, sizeof(event));
            observation.bindingsChanged |= event.session == session;
        } else if (buffer.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            XrEventDataReferenceSpaceChangePending event;
            std::memcpy(&event, &buffer, sizeof(event));
            if (event.session == session) {
                native_->RevokePassthrough();
                auto evidence = ReadReferenceChange(event);
                if (evidence.HasError())
                    return Result<void>::Failure(evidence.ErrorValue());
                if (evidence.Value())
                    observation.referenceChange = evidence.Value();
            }
        } else {
            native_->ObservePassthroughEvent(buffer);
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::Poll */
    Result<NativeSessionEventObservation> OpenXRSessionEvents::Poll(const bool wantsPlay) {
        using Outcome = Result<NativeSessionEventObservation>;
        if (std::this_thread::get_id() != lane_ || dispatching_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (loss_ != NativeSessionEventLoss::None)
            return Outcome::Success({.loss = loss_});
        DispatchScope scope{dispatching_};
        if (!wantsPlay)
            native_->RevokePassthrough();
        auto current = Current();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        NativeSessionEventObservation observation;
        for (std::uint32_t index = 0; index < MaximumEventsPerPoll; ++index) {
            XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
            const auto result = dispatch_.poll(current.Value().instance, &buffer);
            if (auto checked = CheckCall("xrPollEvent", result == XR_EVENT_UNAVAILABLE ? XR_SUCCESS : result); checked.HasError())
                return Outcome::Failure(checked.ErrorValue());
            if (result == XR_SUCCESS) {
                if (auto applied = ProcessEvent(buffer, current.Value().session, observation); applied.HasError())
                    return Outcome::Failure(applied.ErrorValue());
            }
            if (observation.referenceChange) {
                // Return one typed notice at a time and force another poll to prove the event queue drained.
                observation.morePending = true;
                return Outcome::Success(observation);
            }
            if (loss_ == NativeSessionEventLoss::None && !observation.exitRequested) {
                // Begin promptly after the actual READY event before consuming later native running-state events.
                if (auto progressed = Progress(wantsPlay); progressed.HasError())
                    return Outcome::Failure(progressed.ErrorValue());
            }
            observation.loss = loss_;
            if (result == XR_EVENT_UNAVAILABLE || loss_ != NativeSessionEventLoss::None || observation.exitRequested)
                return Outcome::Success(observation);
        }
        observation.morePending = true;
        return Outcome::Success(observation);
    }

    /** @copydoc OpenXRSessionEvents::Running */
    bool OpenXRSessionEvents::Running() const noexcept {
        return std::this_thread::get_id() == lane_ && running_;
    }
}  // namespace Horo::XR::OpenXRInternal
