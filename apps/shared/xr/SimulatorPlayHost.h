#pragma once
/** @file SimulatorPlayHost.h @brief Shared CPU-only canonical Scene/Play/Input composition for explicit simulator development. */
#include "EditorPlayXrHost.h"
#include "Horo/Editor/XRSimulatorModel.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Runtime {
    struct FrameContext;
}

namespace Horo::Application::Internal {
    struct SimulatorPlayHostComposition final {
        Input::InputRouter &router;
        Runtime::RuntimeDispatchSource dispatch;
        Duration fixedStep;
        Runtime::SceneObjectId trackingRoot;
    };

    /** @brief Application retains this owner through workspace/factory/participant retirement; no native provider is used. */
    class SimulatorPlayHost final : public Editor::IEditorPlayXrHost, public Editor::IXRSimulatorControl {
    public:
        /** @param composition Existing real Input/scheduler parents and explicit authored tracking root.
         * @return Cold adapter; fake runtime is deferred until actual Scene participant preparation. */
        [[nodiscard]] static Result<std::shared_ptr<SimulatorPlayHost>> Create(SimulatorPlayHostComposition composition);
        ~SimulatorPlayHost() override;
        [[nodiscard]] Result<void> AttachInput(const Input::InputActionCollection &input,
                                               const Editor::EditorXrSceneSourcePort &geometry) override;
        [[nodiscard]] Result<void> UpdateScene(const Runtime::RuntimeSceneView &scene) override;
        [[nodiscard]] Result<void> SetPlayState(Editor::EditorPlayState state) override;
        [[nodiscard]] Result<void> DetachInput() override;
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant() override;
        void RevokeInput() noexcept override;
        /** @brief Revoke and sever every Input/Scene source borrow before the application router is destroyed. */
        void ReleaseInputOwner() noexcept;
        [[nodiscard]] Editor::EditorPlayViewSource ViewSource() const noexcept override;
        [[nodiscard]] std::optional<Editor::EditorPlayXrPreview> Preview() const noexcept override;
        /** @param context Exact same scheduler BuildInputSnapshot callback. @param raw Same platform committed raw input.
         * @return One canonical Input commit with synthetic contribution, or original typed error after neutralization. */
        [[nodiscard]] Result<void> BuildInputSnapshot(const Runtime::FrameContext &context, const Input::RawInputSnapshot &raw);
        [[nodiscard]] Editor::XRSimulatorView QuerySimulator() const noexcept override;
        [[nodiscard]] Result<void> ApplySimulator(const Editor::XRSimulatorCommand &command) override;
        [[nodiscard]] Result<void> LoadSimulator(const XR::XRSessionId &session, const XR::XRSimulatorScenario &scenario) override;
        [[nodiscard]] Result<XR::XRSimulatorScenario> CaptureSimulator(const XR::XRSessionId &session) const override;

    private:
        struct Impl;
        explicit SimulatorPlayHost(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
