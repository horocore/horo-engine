#include "SimulatorPlayState.h"

namespace Horo::Application::Internal {
    using namespace XR;

    Result<void> SimulatorPlayHost::Impl::Lane() const {
        if (std::this_thread::get_id() != lane || busy)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return Result<void>::Success();
    }

    bool SimulatorPlayHost::Impl::Readable() const noexcept {
        if (std::this_thread::get_id() != lane || busy || !admitted || !scene || !scene->published || !runtime)
            return false;
        try {
            return scene->geometry->ValidateSource().HasValue();
        } catch (...) {
            return false;
        }
    }

    Result<void> SimulatorPlayHost::Impl::Current(const XRSessionId &session) const {
        if (auto owner = Lane(); owner.HasError())
            return owner;
        if (!admitted || !scene || !scene->published || !runtime || runtime->Snapshot().session.session != session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return scene->geometry->ValidateSource();
    }

    void SimulatorPlayHost::Impl::Adopt() noexcept {
        if (candidateInput)
            activeInput = std::move(candidateInput);
        preview.reset();
    }

    void SimulatorPlayHost::Impl::Revoke() noexcept {
        admitted = false;
        pendingStep = false;
        preview.reset();
    }

    void SimulatorPlayHost::Impl::Retire(const Runtime::SceneRuntimeId identity) noexcept {
        if (!scene || scene->identity != identity)
            return;
        Revoke();
        scene->geometry->Revoke();
        runtime.reset();
        candidateInput.reset();
        activeInput.reset();
        retired = identity;
        scene.reset();
    }

    SimulatorPlayHost::SimulatorPlayHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc SimulatorPlayHost::Create */
    Result<std::shared_ptr<SimulatorPlayHost>> SimulatorPlayHost::Create(SimulatorPlayHostComposition composition) {
        if (!composition.dispatch.IsValid() || composition.fixedStep.ToNanoseconds() <= 0 || !composition.trackingRoot.IsValid())
            return Result<std::shared_ptr<SimulatorPlayHost>>::Failure(MakeError(XRErrors::OperationInvalid));
        auto impl = std::make_unique<Impl>(std::move(composition));
        return Result<std::shared_ptr<SimulatorPlayHost>>::Success(
            std::shared_ptr<SimulatorPlayHost>{new SimulatorPlayHost(std::move(impl))});
    }

    SimulatorPlayHost::~SimulatorPlayHost() {
        RevokeInput();
        if (impl_->scene)
            impl_->Retire(impl_->scene->identity);
    }

    /** @copydoc Editor::IEditorPlayXrHost::AttachInput */
    Result<void> SimulatorPlayHost::AttachInput(const Input::InputActionCollection &input,
                                                const Editor::EditorXrSceneSourcePort &geometry) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        if (impl_->input)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        impl_->input = input;
        impl_->source = geometry;
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorPlayXrHost::DetachInput */
    Result<void> SimulatorPlayHost::DetachInput() {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        impl_->Revoke();
        if (impl_->scene)
            impl_->Retire(impl_->scene->identity);
        impl_->input.reset();
        impl_->source.reset();
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorPlayXrHost::RevokeInput */
    void SimulatorPlayHost::RevokeInput() noexcept {
        if (std::this_thread::get_id() == impl_->lane)
            impl_->Revoke();
    }

    /** @copydoc SimulatorPlayHost::ReleaseInputOwner */
    void SimulatorPlayHost::ReleaseInputOwner() noexcept {
        if (std::this_thread::get_id() != impl_->lane || impl_->busy)
            return;
        impl_->Revoke();
        if (impl_->scene)
            impl_->Retire(impl_->scene->identity);
        impl_->input.reset();
        impl_->source.reset();
    }

    /** @copydoc Editor::IEditorPlayXrHost::SetPlayState */
    Result<void> SimulatorPlayHost::SetPlayState(const Editor::EditorPlayState state) {
        using enum Horo::Editor::EditorPlayState;
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        impl_->play = state;
        if (!impl_->runtime)
            return Result<void>::Success();
        if (state == Playing)
            return impl_->runtime->Run();
        if (state == Paused)
            return impl_->runtime->Pause();
        if (state == Starting)
            return Result<void>::Success();
        impl_->Revoke();
        return Result<void>::Success();
    }

    /** @copydoc Editor::IEditorPlayXrHost::ViewSource */
    Editor::EditorPlayViewSource SimulatorPlayHost::ViewSource() const noexcept {
        return Editor::EditorPlayViewSource::Simulator;
    }

    /** @copydoc Editor::IEditorPlayXrHost::Preview */
    std::optional<Editor::EditorPlayXrPreview> SimulatorPlayHost::Preview() const noexcept {
        if (!impl_->Readable())
            return {};
        if (const auto current = impl_->runtime->Snapshot();
            current.session.session != impl_->previewSession || current.relativeNanoseconds != impl_->previewTime)
            return {};
        return impl_->preview;
    }

    /** @copydoc Editor::IXRSimulatorControl::QuerySimulator */
    Editor::XRSimulatorView SimulatorPlayHost::QuerySimulator() const noexcept {
        if (!impl_->Readable())
            return {};
        return {true, impl_->runtime->Snapshot(), impl_->runtime->Controls()};
    }

    Result<void> SimulatorPlayHost::Impl::ResetInteractive() {
        if (auto reset = runtime->Reset(); reset.HasError())
            return reset;
        auto controls = runtime->Controls();
        controls.actionCount = 1;
        controls.actions[0] = {0, XRTrackedDeviceRole::LeftController, XRTrackedActionKind::Vector2, true, 0, 0};
        return runtime->SetControls(controls);
    }

    Result<void> SimulatorPlayHost::Impl::QueueStep() {
        if (play != Editor::EditorPlayState::Paused)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto paused = runtime->Pause(); paused.HasError())
            return paused;
        pendingStep = true;
        return Result<void>::Success();
    }

    Result<void> SimulatorPlayHost::Impl::ResumeAdmission() {
        pendingStep = false;
        return play == Editor::EditorPlayState::Playing ? runtime->Run() : runtime->Pause();
    }

    Result<void> SimulatorPlayHost::Impl::Apply(const Editor::XRSimulatorCommand &command) {
        using enum Editor::XRSimulatorCommandKind;
        switch (command.kind) {
            case Controls:
                return runtime->SetControls(command.controls);
            case Step:
                return QueueStep();
            case Reset:
                return ResetInteractive();
            case Replay:
                return runtime->Replay();
            case ArmCapture:
                return runtime->ArmCapture();
            case StopCapture:
                return runtime->StopCapture();
            case CancelCapture:
                runtime->CancelCapture();
                return Result<void>::Success();
            case Count:
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
    }

    /** @copydoc Editor::IXRSimulatorControl::ApplySimulator */
    Result<void> SimulatorPlayHost::ApplySimulator(const Editor::XRSimulatorCommand &command) {
        if (auto current = impl_->Current(command.expectedSession); current.HasError())
            return current;
        Impl::Operation operation{*impl_};
        auto applied = impl_->Apply(command);
        if (applied.HasValue() && impl_->candidateInput) {
            impl_->Adopt();
            return impl_->ResumeAdmission();
        }
        return applied;
    }

    /** @copydoc Editor::IXRSimulatorControl::LoadSimulator */
    Result<void> SimulatorPlayHost::LoadSimulator(const XRSessionId &session, const XRSimulatorScenario &scenario) {
        if (auto current = impl_->Current(session); current.HasError())
            return current;
        Impl::Operation operation{*impl_};
        auto loaded = impl_->runtime->LoadScenario(scenario);
        if (loaded.HasValue())
            impl_->Adopt();  // Preserve the imported runtime's paused admission until explicit Replay.

        return loaded;
    }

    /** @copydoc Editor::IXRSimulatorControl::CaptureSimulator */
    Result<XRSimulatorScenario> SimulatorPlayHost::CaptureSimulator(const XRSessionId &session) const {
        if (auto current = impl_->Current(session); current.HasError())
            return Result<XRSimulatorScenario>::Failure(current.ErrorValue());
        return impl_->runtime->RecordedScenario();
    }
}  // namespace Horo::Application::Internal
