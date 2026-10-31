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
    Result<void> OpenXRSessionEvents::StateChanged(const XrEventDataSessionStateChanged &event) {
        auto current = Current();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (event.session != current.Value().session)
            return Result<void>::Success();
        if (event.state <= XR_SESSION_STATE_UNKNOWN || event.state > XR_SESSION_STATE_EXITING)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
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
                return sessions_->ApplyEvent(session_, SessionLost);
            default:
                return Result<void>::Success();
        }
    }

    /** @copydoc OpenXRSessionEvents::Progress */
    Result<void> OpenXRSessionEvents::Progress(const bool wantsPlay) {
        auto current = Current();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (!running_ && wantsPlay && observed_ == XR_SESSION_STATE_READY && sessions_->Snapshot().state == XRSessionState::Ready) {
            const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            const auto result = dispatch_.begin(current.Value().session, &begin);
            if (XR_FAILED(result))
                return Result<void>::Failure(Failure("xrBeginSession", result));
            running_ = true;
            exitRequested_ = false;
            if (auto fence = Current(); fence.HasError())
                return Result<void>::Failure(fence.ErrorValue());
            return sessions_->ApplyEvent(session_,
                                         result == XR_SESSION_LOSS_PENDING ? XRSessionEvent::SessionLost : XRSessionEvent::Started);
        }
        if (running_ && !wantsPlay && !exitRequested_ && observed_ != XR_SESSION_STATE_STOPPING) {
            const auto result = dispatch_.requestExit(current.Value().session);
            if (XR_FAILED(result))
                return Result<void>::Failure(Failure("xrRequestExitSession", result));
            exitRequested_ = true;
            if (auto fence = Current(); fence.HasError())
                return Result<void>::Failure(fence.ErrorValue());
        }
        if (running_ && observed_ == XR_SESSION_STATE_STOPPING && ImagesRetired()) {
            const auto result = dispatch_.end(current.Value().session);
            if (XR_FAILED(result))
                return Result<void>::Failure(Failure("xrEndSession", result));
            running_ = false;
            exitRequested_ = false;
            if (auto fence = Current(); fence.HasError())
                return Result<void>::Failure(fence.ErrorValue());
            return sessions_->ApplyEvent(session_, result == XR_SESSION_LOSS_PENDING ? XRSessionEvent::SessionLost : XRSessionEvent::Idle);
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::ProcessEvent */
    Result<void> OpenXRSessionEvents::ProcessEvent(const XrEventDataBuffer &buffer, const XrSession_T *session,
                                                   NativeSessionEventObservation &observation) {
        if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged state;
            std::memcpy(&state, &buffer, sizeof(state));
            if (auto changed = StateChanged(state); changed.HasError())
                return Result<void>::Failure(changed.ErrorValue());
            observation.exitRequested |= state.session == session && state.state == XR_SESSION_STATE_EXITING;
        } else if (buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            if (auto lost = sessions_->ApplyEvent(session_, XRSessionEvent::InstanceLost); lost.HasError())
                return Result<void>::Failure(lost.ErrorValue());
        } else if (buffer.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            // Missing state transitions cannot authorize continued frames or a later Ready restart.
            // Logical loss retains native frame/image debt; it does not manufacture end/release success.
            if (auto lost = sessions_->ApplyEvent(session_, XRSessionEvent::SessionLost); lost.HasError())
                return Result<void>::Failure(lost.ErrorValue());
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "OpenXR event loss revoked session admission"));
        } else if (buffer.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            XrEventDataInteractionProfileChanged event;
            std::memcpy(&event, &buffer, sizeof(event));
            observation.bindingsChanged |= event.session == session;
        } else if (buffer.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            XrEventDataReferenceSpaceChangePending event;
            std::memcpy(&event, &buffer, sizeof(event));
            observation.originChanged |= event.session == session;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSessionEvents::Poll */
    Result<NativeSessionEventObservation> OpenXRSessionEvents::Poll(const bool wantsPlay) {
        using Outcome = Result<NativeSessionEventObservation>;
        if (std::this_thread::get_id() != lane_ || dispatching_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        DispatchScope scope{dispatching_};
        auto current = Current();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        NativeSessionEventObservation observation;
        for (std::uint32_t index = 0; index < 64; ++index) {
            XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
            const auto result = dispatch_.poll(current.Value().instance, &buffer);
            if (XR_FAILED(result))
                return Outcome::Failure(Failure("xrPollEvent", result));
            if (auto fence = Current(); fence.HasError())
                return Outcome::Failure(fence.ErrorValue());
            if (result != XR_SUCCESS && result != XR_EVENT_UNAVAILABLE)
                return Outcome::Failure(Failure("xrPollEvent", result));
            if (result == XR_EVENT_UNAVAILABLE) {
                if (auto progressed = Progress(wantsPlay); progressed.HasError())
                    return Outcome::Failure(progressed.ErrorValue());
                return Outcome::Success(observation);
            }
            if (auto applied = ProcessEvent(buffer, current.Value().session, observation); applied.HasError())
                return Outcome::Failure(applied.ErrorValue());
            // Begin promptly after the actual READY event before consuming later native running-state events.
            if (auto progressed = Progress(wantsPlay); progressed.HasError())
                return Outcome::Failure(progressed.ErrorValue());
        }
        observation.morePending = true;
        return Outcome::Success(observation);
    }

    /** @copydoc OpenXRSessionEvents::Running */
    bool OpenXRSessionEvents::Running() const noexcept {
        return std::this_thread::get_id() == lane_ && running_;
    }
}  // namespace Horo::XR::OpenXRInternal
