#include "OpenXRPlayHost.h"

#include "Horo/Foundation/Assertions.h"

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRPlayHost::OpenXRPlayHost */
    OpenXRPlayHost::OpenXRPlayHost(OpenXRPlayHostComposition composition, OpenXRApplicationPreparation preparation)
        : composition_{composition.session,
                       composition.viewport,
                       composition.frontend,
                       std::move(composition.bindings),
                       std::move(composition.dispatch),
                       composition.fixedStep,
                       composition.trackingRoot},
          native_(preparation.native), getProc_(preparation.getProc), clock_(native_) {
        input_.router = &composition.router;
    }

    /** @copydoc OpenXRPlayHost::Create */
    Result<std::unique_ptr<OpenXRPlayHost>> OpenXRPlayHost::Create(OpenXRPlayHostComposition composition) {
        if (!composition.bindings || !composition.dispatch.IsValid() || composition.fixedStep.ToNanoseconds() <= 0 ||
            !composition.trackingRoot.IsValid())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto prepared = composition.session.PrepareSession();
        if (prepared.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(prepared.ErrorValue());
        const auto facts = composition.session.BorrowPreparation();
        if (facts.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(facts.ErrorValue());
        std::unique_ptr<OpenXRPlayHost> host{new OpenXRPlayHost(std::move(composition), facts.Value())};
        if (auto clock = host->clock_.PrepareScheduler(host->composition_.dispatch, host->composition_.fixedStep); clock.HasError())
            return Result<std::unique_ptr<OpenXRPlayHost>>::Failure(clock.ErrorValue());
        return Result<std::unique_ptr<OpenXRPlayHost>>::Success(std::move(host));
    }

    OpenXRPlayHost::~OpenXRPlayHost() {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_ && !busy_ && !scene_ && !render_ && !events_,
                           "XR Play owner requires explicit physical Scene/frame/native retirement before destruction");
    }

    /** @copydoc OpenXRPlayHost::Close */
    Result<bool> OpenXRPlayHost::Close() {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        closing_ = true;
        if (scene_)
            RetireScene(scene_);
        RevokeInput();
        const Operation operation{*this};
        if (scene_)
            return ProgressRetirement();
        auto retired = composition_.session.RetirePreparation();
        if (retired.HasError())
            return Result<bool>::Failure(retired.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRPlayHost::Lane */
    Result<void> OpenXRPlayHost::Lane() const {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (closing_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FinishOperation */
    void OpenXRPlayHost::FinishOperation() noexcept {
        busy_ = false;
        if (pendingRevoke_) {
            pendingRevoke_ = false;
            composition_.session.RevokeInput();
        }
        if (pendingDetach_) {
            pendingDetach_ = false;
            input_.port.reset();
            input_.geometry.reset();
        }
    }

    /** @copydoc OpenXRPlayHost::AttachInput */
    Result<void> OpenXRPlayHost::AttachInput(const Input::InputActionCollection &input, const Editor::EditorXrSceneSourcePort &geometry) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!input_.router || input_.port)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        input_.port.emplace(input);
        input_.geometry.emplace(geometry);
        // The actual Scene participant reserves native actions/Input against this port and its exact origin.
        // Merely entering a workspace cannot manufacture a Ready session or a semantic registration.
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::SetPlayState */
    Result<void> OpenXRPlayHost::SetPlayState(const Editor::EditorPlayState state) {
        if (auto lane = Lane(); lane.HasError()) {
            return lane;
        }
        if (state > Editor::EditorPlayState::Failed) {
            RevokeSource();
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        input_.play = state;
        if (state == Editor::EditorPlayState::Idle || state == Editor::EditorPlayState::Stopping ||
            state == Editor::EditorPlayState::Failed)
            RevokeSource();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::RevokeSource */
    void OpenXRPlayHost::RevokeSource() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_, "XR Play revocation requires its owner lane");
        input_.admitted = false;
        tracking_.Revoke();
        if (busy_) {
            pendingRevoke_ = true;
            return;
        }
        composition_.session.RevokeInput();
    }

    /** @copydoc OpenXRPlayHost::RevokeInput */
    void OpenXRPlayHost::RevokeInput() noexcept {
        RevokeSource();
        if (busy_) {
            pendingDetach_ = true;
            return;
        }
        input_.port.reset();
        input_.geometry.reset();
    }

    /** @copydoc OpenXRPlayHost::DetachInput */
    Result<void> OpenXRPlayHost::DetachInput() {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        RevokeInput();
        return busy_ ? Result<void>::Failure(MakeError(XRErrors::OperationIncompatible)) : Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::PollEvents */
    Result<void> OpenXRPlayHost::PollEvents() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        const Operation operation{*this};
        if (sessions_.Snapshot().state == XRSessionState::Lost && scene_)
            RetireScene(scene_);
        if (auto tail = ProgressTail(); tail.HasError()) {
            RevokeEventAdmission();
            return tail;
        }
        if (scene_ && scene_->retiring) {
            const auto retired = ProgressRetirement();
            if (retired.HasError())
                return Result<void>::Failure(retired.ErrorValue());
            if (!retired.Value())
                return Result<void>::Success();
        }
        if (!eventsBound_)
            return Result<void>::Success();
        const bool wantsPlay =
            input_.admitted && (input_.play == Editor::EditorPlayState::Starting || input_.play == Editor::EditorPlayState::Playing ||
                                input_.play == Editor::EditorPlayState::Paused);
        const auto observed = events_->Poll(wantsPlay);
        if (observed.HasError()) {
            RevokeEventAdmission();
            return Result<void>::Failure(observed.ErrorValue());
        }
        eventsPending_ = observed.Value().morePending;
        if (sessions_.Snapshot().state == XRSessionState::Lost) {
            if (scene_)
                RetireScene(scene_);
            return Result<void>::Success();
        }
        if (observed.Value().bindingsChanged || observed.Value().originChanged || observed.Value().exitRequested) {
            RevokeEventAdmission();
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::RevokeEventAdmission */
    void OpenXRPlayHost::RevokeEventAdmission() noexcept {
        const auto state = sessions_.Snapshot();
        if (state.session.IsValid()) {
            // The copied publication is current on this serialized lane. SessionLost is legal from
            // every published state and only revokes admission; it never destroys native children.
            const auto lost = sessions_.ApplyEvent(state.session, XRSessionEvent::SessionLost);
            HORO_INVARIANT_MSG(lost.HasValue(), "Current XR event failure must revoke canonical frame admission");
        }
        if (scene_)
            RetireScene(scene_);
        else
            RevokeSource();
    }

    /** @copydoc OpenXRPlayHost::AppendInput */
    Result<void> OpenXRPlayHost::AppendInput(const Runtime::FrameContext &context, Input::InputCollectionFrame &frame) {
        if (!input_.coordinator)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return composition_.session.AppendInput(sessions_, *input_.coordinator, tracking_, clock_, context, input_.origin, frame);
    }

    /** @copydoc OpenXRPlayHost::BuildInputSnapshot */
    Result<void> OpenXRPlayHost::BuildInputSnapshot(const Runtime::FrameContext &context, const Input::RawInputSnapshot &raw) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!input_.router)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{*this};
        if (!input_.port || !input_.admitted || eventsPending_ || input_.play != Editor::EditorPlayState::Playing) {
            tracking_.Revoke();
            input_.router->BeginFrame(raw);
            return Result<void>::Success();
        }
        auto opened = input_.port->Begin(raw);
        if (opened.HasError()) {
            RevokeSource();
            input_.router->BeginFrame(raw);
            return Result<void>::Failure(opened.ErrorValue());
        }
        auto frame = std::move(opened).Value();
        const auto sampled = AppendInput(context, frame);
        if (pendingRevoke_) {
            const auto canceled = frame.Cancel();
            if (canceled.HasError())
                return canceled;
            input_.router->BeginFrame(raw);
            return sampled.HasError() ? sampled : Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        }
        // One empty-source commit on native failure preserves physical edges and neutralizes old XR values.
        const auto committed = frame.Commit();
        if (committed.HasError()) {
            RevokeSource();
            return committed;
        }
        if (sampled.HasError()) {
            RevokeSource();
            return sampled;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ReleaseInputOwner */
    void OpenXRPlayHost::ReleaseInputOwner() noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == lane_ && !busy_, "XR Input owner release requires its idle owner lane");
        RevokeInput();
        input_.coordinator.reset();
        input_.play = Editor::EditorPlayState::Idle;
        input_.router = nullptr;
    }
}  // namespace Horo::Application::Internal
