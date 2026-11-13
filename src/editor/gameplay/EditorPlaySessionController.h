#pragma once

/**
 * @file EditorPlaySessionController.h
 * @brief Isolated editor play-session state machine and runtime-scene ownership.
 */

#include "EditorPlayModulePrograms.h"
#include "EditorPlaySaveRetirement.h"
#include "EditorPlaySceneHost.h"
#include "EditorPlayState.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Gameplay/BehaviorRuntimeRestore.h"
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

    class IEditorPlayXrHost;

    /** @brief Owns one isolated Scene service host and gameplay runner without mutating the authoring document. */
    class EditorPlaySessionController final {
    public:
        /** @brief Borrows optional host diagnostics for the complete Play controller lifetime.
         * @param debugger Host service; must outlive this controller. */
        explicit EditorPlaySessionController(Application::NetworkDebuggerService *debugger = nullptr) noexcept : debugger_(debugger) {}

        /** @brief Stops publication before releasing the clone. */
        ~EditorPlaySessionController();

        /** @brief Admits isolated Play preparation from one committed authoring snapshot.
         * @param authoring Committed immutable document. @param registry Frozen registry outliving pending preparation and active Play.
         * @param factory Explicit application composition; absence supports assetless private callers only.
         * @param viewSource Explicit authored-camera or selected-XR behavior.
         * @param selectedXrHost Exact actual factory participant, mandatory only for selected XR; outlives Play.
         * @param triggerBinding Exact conversion capture. @return Success for accepted preparation or typed rejection.
         * @details Assetless preparation may finish immediately. Required assets retain Starting until owner update pumping completes.
         */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         const EditorPlaySceneFactory &factory = {},
                                         const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding = {},
                                         EditorPlayViewSource viewSource = EditorPlayViewSource::AuthoredCamera,
                                         const IEditorPlayXrHost *selectedXrHost = nullptr);
        /**
         * @brief Creates and starts an isolated runtime clone after gating authored gameplay components against a module snapshot.
         * @param authoring Immutable committed authoring snapshot.
         * @param registry Frozen behavior registry for the active project generation.
         * @param components Frozen component descriptor registry for the active project generation.
         * @param factory Explicit host composition; absence admits only assetless private callers with no optional native services.
         * @param triggerBinding Exact project capture used when conversion is required.
         * @param viewSource Explicit authored-camera or selected-XR behavior.
         * @param selectedXrHost Exact factory participant identity, mandatory for selected XR; outlives Play.
         * @return Accepted preparation or a typed Play-gate/conversion/runtime activation error.
         * @pre Registry/module ownership outlives pending preparation and Play; reload waits until readiness.
         * @details No normal-frame wait: presentation/fixed updates pump bounded Scene progress while Starting.
         * Stop/failure cancels accepted work and closes its native admission before releasing the private Scene.

         */
        [[nodiscard]] Result<void> Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                         const Gameplay::ComponentRegistry &components, const EditorPlaySceneFactory &factory = {},
                                         const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding = {},
                                         EditorPlayViewSource viewSource = EditorPlayViewSource::AuthoredCamera,
                                         const IEditorPlayXrHost *selectedXrHost = nullptr);
        /** @brief Selects explicit application lifetime storage before activation. No queue is discovered implicitly.
         * @param owner Cold bounded queue outliving the controller and actual scheduler join. */
        void ConfigureSaveRetirement(std::shared_ptr<EditorPlaySaveRetirement> owner) noexcept;
        /** @brief Revokes binding and transfers an unsettled exact Play unit into reserved application ownership.
         * @param registry Native project registry whose borrowed behavior metadata must outlive the moved runtime.
         * @details Used only at workspace leave after Stop. A settled unit leaves registry untouched. */
        void RetireOnLeave(std::unique_ptr<ProjectGameplayRegistry> &registry) noexcept;
        /** @brief Borrows only the selected activation's genuine save owner for synchronous UI intent. */
        [[nodiscard]] EditorPlaySaveSession *SaveSession() noexcept;
        /** @brief Retains path-free UI intent storage without exposing a controller pointer or extending callback admission. */
        [[nodiscard]] std::weak_ptr<EditorPlayRuntimeBinding> SaveBinding() const noexcept;
        /** @brief Captures exact privately issued installed sources cold before the real Play host is activated. */
        void ConfigureSaveBindings(std::vector<PlaySaveGameplayBinding> bindings);
        /** @brief Preserves whole-inventory admission failure; no empty core-only save is substituted for missing required sources. */
        void ConfigureSaveError(Error error);
        /** @brief Revokes UI save borrowing and releases old-generation sources only after actual worker/delegate settlement. */
        [[nodiscard]] bool PrepareSaveRetirement() noexcept;
        /** Explicit product module-program composition; only configurable before Play. No automatic source execution. */
        [[nodiscard]] Result<void> ConfigureModuleScripts(std::shared_ptr<Extensions::ScriptModuleHostServices>);
        /** Cold admission from an actual authenticated source capture, current native evidence and explicit project/context consent. */
        [[nodiscard]] Result<void> BeginModuleScript(std::shared_ptr<const Extensions::ScriptModuleProgram>,
                                                     Extensions::ScriptModuleOperationPolicy, const CancellationToken & = {});
        [[nodiscard]] std::optional<Extensions::ScriptCallResult> TakeModuleScriptResult();
        [[nodiscard]] Result<void> RequestModuleReferenceUpdate(Extensions::ScriptExportVersion);
        [[nodiscard]] Result<Extensions::ScriptModuleUpdateReport> RequestNativeModuleUpdate(const Extensions::ScriptModuleProgram &);
        [[nodiscard]] std::optional<Extensions::ScriptModuleUpdateReport> TakeModuleUpdateReport();
        /** @brief Pauses fixed simulation while leaving presentation active. */
        [[nodiscard]] Result<void> Pause();
        /** @brief Resumes continuous fixed simulation. */
        [[nodiscard]] Result<void> Resume();
        /** @brief Queues exactly one fixed tick while paused. */
        [[nodiscard]] Result<void> Step();
        /** @brief Shuts down behaviors and destroys the isolated runtime clone. */
        void Stop() noexcept;
        /** @brief Legacy stepping for unbound private callers; a bound real host must supply its actual fixed context. */
        [[nodiscard]] Result<void> FixedUpdate(std::span<const Gameplay::GameplayInputAction> input, Gameplay::FixedDeltaTime delta);
        /** @brief Binds one genuine live issuer cold; permits active-frame composition without granting callback admission. */
        [[nodiscard]] Result<void> BindRuntimeDispatch(const Runtime::RuntimeDispatchSource &source);
        /** @brief Stages one actual host attempt; paused step remains pending until its real commit. */
        [[nodiscard]] Result<void> OnRuntimeFixedUpdate(std::span<const Gameplay::GameplayInputAction> input,
                                                        const Runtime::FixedStepContext &context);
        [[nodiscard]] Result<void> PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context);
        void CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept;
        void AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept;
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
        [[nodiscard]] bool ShouldTick() const noexcept;
        [[nodiscard]] Result<void> RunGameplayFixed(std::span<const Gameplay::GameplayInputAction> input, Gameplay::FixedDeltaTime delta);
        [[nodiscard]] Result<void> RejectFixedAttempt(const Runtime::FixedStepContext &context, Error error);
        [[nodiscard]] bool MatchesFixedAttempt(const Runtime::FixedStepContext &context) const noexcept;
        void FlushPendingStop() noexcept;

        struct FixedAttempt final {
            EditorPlaySceneHost *scene{};
            Runtime::RuntimeDispatchEvidence evidence;
            Runtime::RuntimeDispatchFacts facts;
            bool consumesStep{};
            bool prepared{};
        };

        Runtime::RuntimeDispatchSource dispatchSource_;
        std::optional<FixedAttempt> fixedAttempt_;
        bool stopPending_{};
        EditorPlayViewSource viewSource_{EditorPlayViewSource::AuthoredCamera};
        const IEditorPlayXrHost *selectedXrHost_{};
        /** @brief Reserves leave capacity and a callback lookup before any Scene activation is admitted. */
        [[nodiscard]] Result<void> ReservePlayOwnership();
        /** @brief Acquires one configured owner and admits its asynchronous Scene preparation without waiting. */
        [[nodiscard]] Result<void> BeginPreparation(Runtime::RuntimeSceneDefinition definition, const Gameplay::BehaviorRegistry &registry,
                                                    const EditorPlaySceneFactory &factory);
        /** @brief Starts the configured Scene and retains the actual pending registry through activation.
         * @param definition Validated owned Play definition. @param registry Live frozen registry retained by the Play unit.
         * @return Original Scene startup/preparation failure or genuine queue admission. */
        [[nodiscard]] Result<void> StartPreparedScene(Runtime::RuntimeSceneDefinition definition,
                                                      const Gameplay::BehaviorRegistry &registry);
        /** @brief Registers the canonical Scene-owned runtime before any Scene
         * preparation is admitted. */
        [[nodiscard]] Result<void> PrepareBehaviorOwner(const Gameplay::BehaviorRegistry &registry);
        /** @brief Refreshes only the active borrow after Scene publication; owns no
         * second runtime or commit gate. */
        void RefreshBehaviorBinding();
        /** @brief Advances only owned asynchronous Scene preparation and admits behaviors after readiness. */
        [[nodiscard]] Result<void> AdvancePreparation();
        /** @brief Commits the isolated Play host's queued commands after callbacks close and before native stepping. */
        [[nodiscard]] Result<void> CommitSceneChanges();
        /** @brief Pumps actual cancellation settlement once, retaining owners on pending work or failure. */
        void PollStopping() noexcept;
        /** @brief Tears down only after genuine detached/Scene work has settled. */
        void FinishStop() noexcept;
        /** @brief Installs available save composition after actual Scene/behavior readiness; unsupported state stays visible to Manager. */
        void ConfigureReadySave();
        /** @brief Drains one path-free UI intent at the owner boundary, using the existing Play pause authority for Load. */
        void DispatchSaveCommand();
        /** @brief Reports genuine immutable load admission state without touching storage or operation ownership. */
        [[nodiscard]] bool SaveLoading() noexcept;
        void Fail(Error error) noexcept;
        void RefreshModuleScriptEpoch();
        void PollModuleScripts();
        void CancelModuleScripts() noexcept;
        std::unique_ptr<EditorPlayModulePrograms> moduleScripts_;
        std::optional<Error> moduleScriptError_;

        Application::NetworkDebuggerService *debugger_{};
        std::uint64_t diagnosticGeneration_{};
        EditorPlaySessionState state_{EditorPlaySessionState::Idle};
        Gameplay::ComponentRegistry prefabComponents_; /**< Owned inert schema snapshot, fixed for the complete Scene epoch. */
        std::unique_ptr<EditorPlaySceneHost> sceneHost_;
        const Gameplay::BehaviorRegistry *pendingRegistry_{}; /**< Frozen module owner remains alive while Starting. */
        CancellationSource preparationCancellation_;
        std::uint64_t preparationFrame_{1};
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner_;
        Gameplay::BehaviorRuntime *behaviors_{}; /**< Owner-lane borrow refreshed
                                                    after the sole Scene commit. */
        std::shared_ptr<EditorPlayRuntimeBinding> runtimeBinding_;
        std::vector<PlaySaveGameplayBinding> saveBindings_;
        std::optional<Error> saveSetupError_;
        std::shared_ptr<EditorPlaySaveRetirement> saveRetirement_;
        EditorPlaySaveRetirement::Reservation retirementReservation_;
        std::optional<Error> lastError_;
        DocumentRevision authoringRevision_{};
        bool stepPending_{false};
        std::uint64_t nextRuntimeId_{EditorFirstPlayScene.value};
    };
}  // namespace Horo::Editor
