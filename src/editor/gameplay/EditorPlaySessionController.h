#pragma once

/**
 * @file EditorPlaySessionController.h
 * @brief Isolated editor play-session state machine and runtime-scene ownership.
 */

#include "EditorPlaySceneHost.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "editor/document/RuntimeSceneConversion.h"

#include <memory>
#include <optional>

namespace Horo::Editor {
    /** @brief Explicit editor play-mode lifecycle state. */
    enum class EditorPlaySessionState : std::uint8_t {
        Idle,
        Starting,
        Playing,
        Paused,
        Reloading,
        Stopping,
        Failed,
    };

    /** @brief Runtime-only play state retained across one native gameplay replacement. */
    struct EditorPlayReloadSnapshot {
        Gameplay::BehaviorRuntimeReloadSnapshot behaviors;
        EditorPlaySessionState priorState{EditorPlaySessionState::Playing};
    };

    /** @brief Owns one isolated Scene service host and gameplay runner without mutating the authoring document. */
    class EditorPlaySessionController final {
    public:
        /** @brief Borrows optional host diagnostics for the complete Play controller lifetime.
         * @param debugger Host service; must outlive this controller. */
        explicit EditorPlaySessionController(Application::NetworkDebuggerService *debugger = nullptr) noexcept : debugger_(debugger) {}

        /** @brief Stops publication before releasing the clone. */
        ~EditorPlaySessionController() {
            Stop();
        }

        /** @brief Admits isolated Play preparation from one committed authoring snapshot.
         * @param authoring Committed immutable document. @param registry Frozen registry outliving pending preparation and active Play.
         * @param factory Explicit application composition; absence supports assetless private callers only.
         * @param triggerBinding Exact conversion capture. @return Success for accepted preparation or typed rejection.
         * @details Assetless preparation may finish immediately. Required assets retain Starting until owner update pumping completes.
         */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         const EditorPlaySceneFactory &factory = {},
                                         const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding = {});
        /**
         * @brief Creates and starts an isolated runtime clone after gating authored gameplay components against a module snapshot.
         * @param authoring Immutable committed authoring snapshot.
         * @param registry Frozen behavior registry for the active project generation.
         * @param components Frozen component descriptor registry for the active project generation.
         * @param factory Explicit host composition; absence admits only assetless private callers with no optional native services.
         * @param triggerBinding Exact project capture used when conversion is required.
         * @return Accepted preparation or a typed Play-gate/conversion/runtime activation error.
         * @pre Registry/module ownership outlives pending preparation and Play; reload waits until readiness.
         * @details No normal-frame wait: presentation/fixed updates pump bounded Scene progress while Starting.
         * Stop/failure cancels accepted work and closes its native admission before releasing the private Scene.

         */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         const Gameplay::ComponentRegistry &components, const EditorPlaySceneFactory &factory = {},
                                         const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding = {});
        /** @brief Pauses fixed simulation while leaving presentation active. */
        [[nodiscard]] Result<void> Pause();
        /** @brief Resumes continuous fixed simulation. */
        [[nodiscard]] Result<void> Resume();
        /** @brief Queues exactly one fixed tick while paused. */
        [[nodiscard]] Result<void> Step();
        /** @brief Shuts down behaviors and destroys the isolated runtime clone. */
        void Stop() noexcept;
        /** @brief Runs one host fixed tick when playing, or consumes one queued paused step. */
        [[nodiscard]] Result<void> FixedUpdate(std::span<const Gameplay::GameplayInputAction> input, Gameplay::FixedDeltaTime delta);
        /** @brief Runs presentation callbacks for an active session. */
        void PresentationUpdate(Gameplay::FrameDeltaTime delta);
        /**
         * @brief Recreates behavior instances against a validated registry without replacing runtime scene state.
         * @param candidate New complete registry whose module/program owners remain alive after success.
         * @param rollback Previous complete registry used if candidate activation fails.
         * @return Success when the candidate activates; failure keeps the previous registry active when rollback succeeds.
         */
        [[nodiscard]] Result<void> ReloadBehaviors(const Gameplay::BehaviorRegistry &candidate, const Gameplay::BehaviorRegistry &rollback);
        /**
         * @brief Captures behavior state and destroys every old-generation behavior at the fixed-tick safe point.
         * @return Reload state or a typed failure that leaves the active runtime intact.
         */
        [[nodiscard]] Result<EditorPlayReloadSnapshot> QuiesceForReload();
        /**
         * @brief Recreates behaviors from a loaded generation and restores the captured runtime-only state.
         * @param registry Frozen behavior registry owned by the active generation.
         * @param snapshot State captured before the old generation was unloaded.
         * @return Success or a typed activation/restore failure while the session remains quiesced.
         */
        [[nodiscard]] Result<void> RestoreAfterReload(const Gameplay::BehaviorRegistry &registry, const EditorPlayReloadSnapshot &snapshot);
        /**
         * @brief Enters a safe failed state when neither replacement nor rollback can restore Play.
         * @param error Actionable terminal reload failure.
         */
        void DegradeAfterReload(Error error) noexcept;

        [[nodiscard]] EditorPlaySessionState State() const noexcept;
        [[nodiscard]] bool IsActive() const noexcept;
        /** @brief Borrows only a ready isolated Play Scene; pending/closed sessions expose no Scene.
         * @return Read-only current view or absence. Never transfers mutable Scene ownership.
         */
        [[nodiscard]] std::optional<Runtime::RuntimeSceneView> SceneView() const noexcept;
        /** @brief Borrows the actual Play host receipt synchronously; absence never implies no Physics. */
        [[nodiscard]] std::optional<EditorSceneCollisionReceipt> CollisionReceipt() const noexcept;
        [[nodiscard]] const std::optional<Error> &LastError() const noexcept;
        [[nodiscard]] DocumentRevision AuthoringRevision() const noexcept;

    private:
        /** @brief Acquires one configured owner and admits its asynchronous Scene preparation without waiting. */
        [[nodiscard]] Result<void> BeginPreparation(Runtime::RuntimeSceneDefinition definition, const Gameplay::BehaviorRegistry &registry,
                                                    const EditorPlaySceneFactory &factory);
        /** @brief Advances only owned asynchronous Scene preparation and admits behaviors after readiness. */
        [[nodiscard]] Result<void> AdvancePreparation();
        /** @brief Commits the isolated Play host's queued commands after callbacks close and before native stepping. */
        [[nodiscard]] Result<void> CommitSceneChanges();
        void Fail(Error error) noexcept;

        Application::NetworkDebuggerService *debugger_{};
        std::uint64_t diagnosticGeneration_{};
        EditorPlaySessionState state_{EditorPlaySessionState::Idle};
        std::unique_ptr<EditorPlaySceneHost> sceneHost_;
        const Gameplay::BehaviorRegistry *pendingRegistry_{}; /**< Frozen module owner remains alive while Starting. */
        CancellationSource preparationCancellation_;
        std::uint64_t preparationFrame_{1};
        std::unique_ptr<Gameplay::BehaviorRuntime> behaviors_;
        std::optional<Error> lastError_;
        DocumentRevision authoringRevision_{};
        bool stepPending_{false};
        std::uint64_t nextRuntimeId_{EditorFirstPlayScene.value};
    };
}  // namespace Horo::Editor
