#pragma once
/** @file SimulatorProjectWorkflow.h @brief Explicit CPU project sample using the shared simulator, canonical scheduler/Input/Scene/Lua
 * owners. */
#include "EditorPlayXrHost.h"
#include "Horo/Editor/XRSimulatorModel.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <filesystem>

namespace Horo::Application::Internal {
    struct SimulatorProjectOptions final {
        std::filesystem::path projectRoot;
        std::string scenePath{"Assets/Scenes/main.horo"};
        std::string scriptPath{"Assets/Scripts/SimulatorMovement.horo_script"};
        Runtime::SceneObjectId trackingRoot{1};
    };

    class SimulatorProjectWorkflow final {
    public:
        /** @param options Actual admitted sample project and relative assets. @return One initialized canonical host or original typed
         * failure. */
        [[nodiscard]] static Result<std::unique_ptr<SimulatorProjectWorkflow>> Create(SimulatorProjectOptions options);
        ~SimulatorProjectWorkflow();
        /** @brief Advance one headless opportunity through the actual RuntimeHost scheduler.
         * @return Original Input/Scene/Gameplay/lifecycle failure; no context or committed tick is fabricated. */
        [[nodiscard]] Result<void> Frame();
        /** @return Actual pause lease admission. */
        [[nodiscard]] Result<void> Pause();
        /** @return Actual pause release and simulator run admission. */
        [[nodiscard]] Result<void> Resume();
        /** @return Actual scheduler single-step receipt and matching simulator sampling admission. */
        [[nodiscard]] Result<void> Step();
        /** @return Borrowed weak-port model, never the Scene-owned fake runtime. */
        [[nodiscard]] Editor::XRSimulatorModel &Model() noexcept;
        /** @return Same-root admitted artifact capability, revoked on workflow shutdown. */
        [[nodiscard]] std::shared_ptr<Editor::IXRSimulatorArtifacts> Artifacts() const noexcept;
        /** @param object Actual authored object in the current published Scene. @return Current canonical local position. */
        [[nodiscard]] Result<Math::Vec3> Position(Runtime::SceneObjectId object) const;
        /** @return Actual current shared host view, never an authored camera fallback. */
        [[nodiscard]] std::optional<Editor::EditorPlayXrPreview> Preview() const noexcept;
        /** @return Actual active Scene occurrence; invalid while no Scene is published. */
        [[nodiscard]] Runtime::SceneRuntimeId SceneIdentity() const noexcept;

    private:
        struct Impl;
        explicit SimulatorProjectWorkflow(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
