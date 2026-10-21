#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Assets/AssetPreviewService.h"
#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorEngineEventBridge.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "Horo/Editor/NotificationService.h"
#include "Horo/Editor/PCGGraphDocumentService.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/UiCanvasDocument.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/project_model/EditorSelectionModel.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"
#include "editor/screens/workspace/prefab/PrefabHierarchySources.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Extensions {
    class EditorInspectorHost;
}

namespace Horo::Editor {
    class ILocalizationService;
    struct EditorViewportPickResult;
    /** @brief Host presentation adapter for one already-validated source-open result. */
    using SourceOpenNavigator = std::function<bool(const SourceOpenResult &)>;
    /** @brief Platform navigation capability for one validated diagnostic source location. */
    using DiagnosticSourceNavigator = std::function<bool(const DiagnosticSourceRequest &)>;

    /** @brief Host-owned collision evidence borrowed for the workspace lifetime; null members mean unavailable. */
    struct EditorWorkspaceCollisionEvidence {
        CollisionInventoryEvidence *inventory{};
        CollisionActivationEvidence *activation{};
        const EditorPreviewCollisionReceiptSource *previewReceipt{};
    };

    /** @brief Explicit gameplay build, play-scene and module-retirement services captured by the application host. */
    struct EditorWorkspaceGameplayServices {
        Application::GameplayBuildService *builds{};
        Application::GameplayBuildEnvironment environment{};
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements;
        EditorPlaySceneFactory sceneFactory{}; /**< Host composition capturing the selected native and asset authorities. */
    };

    /** @brief Optional services used by the workspace without transferring their ownership. */
    struct EditorWorkspaceDependencies {
        Assets::AssetRegistry *mutableAssetRegistry{};
        EditorWorkspaceCollisionEvidence collision{};
        ProjectMutationCoordinator *mutations{};
        DurableFileSystem *durableFiles{};
        std::shared_ptr<const Assets::IAssetRegistrySource> assetRegistrySource;
        const Assets::AssetImporterCatalogSnapshot *importerCatalog{};
        JobSystem *jobs{};
        SourceOpenNavigator sourceOpenNavigator{};
        DiagnosticSourceNavigator diagnosticSourceNavigator{};
        EditorWorkspaceGameplayServices gameplay{};
        const ILocalizationService *localization{};
        EngineDataBus *engineEvents{};
        Application::NetworkDebuggerService *networkDebugger{};
        Assets::AssetId documentSessionOwner;              /**< Host-issued never-reused workspace incarnation; no ambient generation. */
        const PCG::IPCGGraphSourceMigrator *pcgMigrator{}; /**< Explicit host-owned compatible schema converter. */
        Extensions::EditorInspectorHost *inspectorHost{};  /**< Explicit borrowed host, outliving this workspace. */
        IAudioComponentPreview *audioPreview{};            /**< Explicit application-owned output capability; absent means unavailable. */
        PrefabHierarchySourceCapture prefabSources;        /**< Explicit host loader; never discovered by panels. */
        EditorPlaySceneFactory prefabStageFactory;         /**< Explicit isolated native/assets stage composition. */
        PCGDocumentCatalog pcgCatalog{.revision = 1};      /**< Empty support preserves graph nodes inert until a catalog is composed. */
        std::optional<SceneSource::TriggerVolumeConversionContext>
            triggerBinding; /**< Explicit immutable project capture; omitted in Null composition. */
    };

    class EditorWorkspaceController {  // NOSONAR(cpp:S1820, cpp:S1448) Authoritative workspace controller
    public:
        EditorWorkspaceController(const std::filesystem::path &projectRoot, Runtime::RuntimeSceneService &runtimeScene,
                                  const Assets::AssetRegistrySnapshot &assetRegistry = {},
                                  const EditorWorkspaceDependencies &dependencies = {});
        ~EditorWorkspaceController();

        /** @brief Starts captured audio through the injected host, never through a widget/backend bypass.
         * @param object Exact selected authored source. @param revision Expected document revision. @return Typed admission/failure. */
        [[nodiscard]] Result<void> StartAudioPreview(SceneObjectId object, DocumentRevision revision);
        /** @brief Revokes only this workspace's output, retaining native ownership in the application. */
        void StopAudioPreview() noexcept;
        /** @brief Reconciles authoring/selection changes and copies current typed preview facts. */
        void ReconcileAudioPreview();
        /** @brief Opens cached project collision authoring bound to this exact canonical Scene opening.
         * @return Shared owner or typed unavailable/source failure; presentation retains weak references only. */
        [[nodiscard]] Result<std::shared_ptr<CollisionProjectSettingsSession>> CollisionSettings();
        /** @brief Includes collision semantics and uncertain durability in the workspace leave guard. */
        [[nodiscard]] bool HasDirtyCollisionSettings() const noexcept;
        /** @brief Saves the exact collision draft with complete reference review. @return Original typed outcome. */
        [[nodiscard]] Result<void> SaveCollisionSettings() const;
        /** @brief Revokes modal access before Scene owners retire. @param discardDirty Explicit discard decision.
         * @return Unchanged dirty rejection or successful owner shutdown. */
        [[nodiscard]] Result<void> ShutdownCollisionSettings(bool discardDirty = false);

        /** @brief Starts a camera-only authored preview using the same runtime hard-cut composition as a packaged view.
         * @param activation Owned camera-only plan and player descriptor. @param cuts Required camera bindings/end policy.
         * @param view Host-issued generation-safe preview view. @return Success or typed admission failure. */
        [[nodiscard]] Result<void> StartCameraCutPreview(Cinematic::SequencePlaybackActivation activation,
                                                         const Cinematic::CameraCutActivation &cuts, Runtime::CameraViewContextId view);
        /** @brief Advances the preview with exact owner-clock ticks before viewport extraction.
         * @param delta Non-negative exact timeline delta. @return Success or typed runtime/binding failure. */
        [[nodiscard]] Result<void> AdvanceCameraCutPreview(Cinematic::SequenceTime delta);
        /** @brief Seeks without event traversal and immediately re-extracts the final camera.
         * @param position Exact authored timeline target. @return Success or typed seek/binding failure. */
        [[nodiscard]] Result<void> SeekCameraCutPreview(Cinematic::SequenceTime position);
        /** @brief Releases preview authority at the owner boundary and restores the current authoring camera. */
        void StopCameraCutPreview();

        [[nodiscard]] const EditorWorkspaceViewModel &ViewModel() const noexcept {
            return m_viewModel;
        }

        /** @brief Returns the workspace-owned typed source query/edit capability, independently from widgets.
         * @return Owner-thread document service; workers receive immutable snapshots only.
         */
        [[nodiscard]] SourceDocumentService &SourceDocuments() noexcept {
            return m_sourceOpenService.Documents();
        }

        /** @brief Refreshes current source roots and checks the workspace leave guard. @return True for any dirty source. */
        [[nodiscard]] bool HasDirtySourceDocuments();
        /** @brief Saves all dirty sources through their owner; uncertain durability keeps navigation blocked.
         * @return Success only after every source is durably clean. */
        [[nodiscard]] Result<void> SaveSourceDocuments();

        /** @brief Last typed single-source save/Save As outcome from the actual workspace command route. */
        [[nodiscard]] const std::optional<Result<SourceSaveResult>> &SourceSaveOutcome() const noexcept {
            return m_sourceSaveOutcome;
        }

        /** @brief Last bounded Save All batch, including every independently failed document. */
        [[nodiscard]] const std::optional<Result<std::vector<SourceSaveAllItem>>> &SourceSaveAllOutcome() const noexcept {
            return m_sourceSaveAllOutcome;
        }

        /** @brief Last explicit source close choice outcome; false retains the document after Cancel. */
        [[nodiscard]] const std::optional<Result<bool>> &SourceCloseOutcome() const noexcept {
            return m_sourceCloseOutcome;
        }

        /** @brief Queries workspace-owned PCG source without lending mutable roots. @return Owner-thread immutable query service. */
        [[nodiscard]] const PCGGraphDocumentService &PCGDocuments() const noexcept {
            return m_pcgDocuments;
        }

        /** @brief Submits the shared typed graph command and reconciles presentation dirty state.
         * @param instance Exact persistent route. @param command Exact semantic intent. @return Typed mutation result. */
        [[nodiscard]] Result<void> ExecutePCGDocument(DocumentInstanceId instance, const PCGDocumentCommand &command);
        /** @brief Replays exact semantic history and reconciles the tab projection.
         * @param instance Exact route. @param command Exact fence. @param redo Direction. @return Typed result. */
        [[nodiscard]] Result<void> ReplayPCGDocument(DocumentInstanceId instance, const PCGDocumentCommand &command, bool redo);
        /** @brief Saves through Assets authority and projects actual saved/dirty state.
         * @param instance Exact route. @param command Exact live fence. @return Durable/reconciliation outcome. */
        [[nodiscard]] Result<Assets::AssetSourcePublication> SavePCGDocument(DocumentInstanceId instance,
                                                                             const PCGDocumentCommand &command);
        /** @brief Closes graph authority only after an explicit save/discard/cancel decision.
         * @param instance Exact route. @param discardDirty Explicit loss decision. @return Typed lifecycle result. */
        [[nodiscard]] Result<void> ClosePCGDocument(DocumentInstanceId instance, bool discardDirty = false);
        /** @brief Includes graph documents in the shared leave guard. @return True if any graph has unsaved semantics. */
        [[nodiscard]] bool HasDirtyPCGDocuments() const;
        /** @brief Reports dirty prefab source sessions for the shared leave workflow. */
        [[nodiscard]] bool HasDirtyPrefabDocuments() const;
        /** @brief Saves every dirty prefab through the canonical source publisher. */
        [[nodiscard]] Result<void> SavePrefabDocuments();
        /** @brief Saves the Scene authority explicitly for the multi-document leave transaction. */
        void SaveScene(bool overwriteConflict = false);
        /** @brief Flushes recovery or explicitly discards before releasing prefab owners. */
        [[nodiscard]] Result<void> ShutdownPrefabDocuments(bool discard = false);
        /** @brief Advances bounded recovery cadence; zero requests an immediate flush. */
        void UpdatePrefabAutosave(float seconds, int minutes);
        /** @brief Saves every dirty graph through its one document service. @return Success only for durable publications. */
        [[nodiscard]] Result<void> SavePCGDocuments();
        /** @brief Drains graph ownership after explicit save/discard/recovery policy.
         * @param discardDirty Explicit leave decision. @return Success or retryable incomplete ownership result. */
        [[nodiscard]] Result<void> ShutdownPCGDocuments(bool discardDirty = false);
        /** @brief Stops play and drains native module retirement before workspace destruction. @return Typed busy if callbacks remain. */
        [[nodiscard]] Result<void> ShutdownGameplayModules();

        /** @brief Lazily composes actual project navigation sources with this exact active Scene/history owner. */
        [[nodiscard]] Result<NavigationProjectSettingsSession *> NavigationSettings();
        /** @brief Includes all navigation authoring owners in the ordinary leave guard. */
        [[nodiscard]] bool HasDirtyNavigationDocuments() const;
        /** @brief Saves each dirty navigation owner through actual Assets publication. */
        [[nodiscard]] Result<void> SaveNavigationDocuments();
        /** @brief Closes only after explicit save/discard policy. */
        [[nodiscard]] Result<void> ShutdownNavigationDocuments(bool discard = false);
        /** @brief Projects committed canonical Scene changes on the existing owner frame boundary. */
        void UpdateNavigationSettingsProjection();

        [[nodiscard]] EditorDataBus &DataBus() noexcept {
            return m_dataBus;
        }

        [[nodiscard]] NotificationService &Notifications() noexcept {
            return m_notifications;
        }

        [[nodiscard]] const NotificationService &Notifications() const noexcept {
            return m_notifications;
        }

        /** @brief Returns the current monotonic authoritative selection revision. */
        [[nodiscard]] SelectionRevision CurrentSelectionRevision() const noexcept {
            return m_selection.Current().revision;
        }

        /** @brief Returns the current monotonic authoritative viewport revision. */
        [[nodiscard]] ViewportRevision CurrentViewportRevision() const noexcept {
            return m_viewport.Current().revision;
        }

        /** @brief Returns the monotonic revision of the extracted viewport scene projection. */
        [[nodiscard]] std::uint64_t CurrentViewportSceneRevision() const noexcept {
            return m_viewportSceneRevision;
        }

        /** @brief Returns the latest owning render snapshot extracted from the authoritative document.
         */
        [[nodiscard]] const EditorViewportSceneSnapshot &ViewportScene() const noexcept {
            return m_activePrefabDocument ? m_prefabViewportScene : m_viewportScene;
        }

        /** @brief Returns the active document's absolute canonical scene path when available. */
        [[nodiscard]] const std::optional<std::filesystem::path> &CurrentScenePath() const noexcept {
            return m_defaultScenePath;
        }

        /** @brief Returns a scene initialization failure that prevents workspace activation. */
        [[nodiscard]] const std::optional<Error> &InitializationError() const noexcept {
            return m_initializationError;
        }

        void ProcessCommand(const EditorWorkspaceViewCommandData &cmd);
        void UpdateFps(float fps);
        /** @brief Replaces the Content Browser projection when a newer registry revision is published.
         */
        void RefreshAssets(const Assets::AssetRegistrySnapshot &assetRegistry);
        /** @brief Advances a requested synchronous Content Browser refresh without blocking the request
         * frame. */
        void UpdateContentBrowser();
        /** @brief Polls project Lua sources and applies compatible safe-point reload candidates. */
        void UpdateGameplaySources(float elapsedSeconds);
        /**
         * @brief Advances dirty-scene autosave scheduling from committed editor policy.
         * @param elapsedSeconds Owner-thread elapsed frame time.
         * @param intervalMinutes Zero disables autosave; positive values schedule recovery writes.
         */
        void UpdateAutosave(float elapsedSeconds, int intervalMinutes);
        /**
         * @brief Schedules and drains non-blocking canonical scene identity inspections.
         * @param elapsedSeconds Owner-thread elapsed frame time.
         */
        void UpdateExternalSceneWatch(float elapsedSeconds);
        /**
         * @brief Captures immutable owner-thread input for a background canonical comparison.
         * @return Absolute canonical location and active document snapshot.
         */
        [[nodiscard]] Result<SceneDocumentComparisonRequest> CaptureExternalSceneComparison() const;
        /** @brief Writes the latest dirty scene to recovery storage at a lifecycle checkpoint. */
        void FlushAutosave();
        /** @brief Publishes a newly activated runtime scene to the cached viewport snapshot. */
        void SynchronizeRuntimeScenePreview();
        /** @brief Advances gameplay presentation callbacks without changing authoring state. */
        void UpdatePlayPresentation(float elapsedSeconds);
        /** @brief Advances one play-mode fixed tick with already-routed semantic actions. */
        void UpdatePlayFixed(std::span<const Gameplay::GameplayInputAction> input, double fixedDeltaSeconds);

    private:
        /** @brief Opens/focuses the exact registered persistent definition asset route. */
        void OpenNavigationDefinitionTab(Assets::AssetId asset);
        /** @brief Projects constant-time dirty/focus/close state without copying Scene roots per frame. */
        void UpdateNavigationDocumentTabs();
        Runtime::RuntimeSceneService &m_runtimeScene;
        std::optional<SceneSource::TriggerVolumeConversionContext> m_triggerBinding;
        EditorPlaySceneFactory m_playSceneFactory;
        Assets::AssetRegistrySnapshot m_assetRegistry;
        DocumentIdentityRegistry m_documentRegistry;
        std::optional<Result<SourceSaveResult>> m_sourceSaveOutcome;
        std::optional<Result<std::vector<SourceSaveAllItem>>> m_sourceSaveAllOutcome;
        std::optional<Result<bool>> m_sourceCloseOutcome;
        SourceFileOpenService m_sourceOpenService;
        PCGGraphDocumentService m_pcgDocuments;
        std::vector<std::unique_ptr<PrefabDocumentSession>> m_prefabDocuments;
        std::optional<DocumentInstanceId> m_activePrefabDocument;
        EditorViewportSceneSnapshot m_prefabViewportScene;
        std::uint64_t m_nextPrefabRuntime{EditorFirstPrefabStage.value};
        float m_prefabAutosaveSeconds{};
        std::uint64_t m_prefabStageAttemptRevision{};
        bool m_prefabRefreshing{};
        bool m_prefabStagePending{};
        std::uint64_t m_prefabStageExtractedRevision{};
        EditorPlaySceneFactory m_prefabStageFactory;
        PrefabHierarchySourceCapture m_prefabSourceCapture;
        Assets::AssetId m_workspaceSession;
        std::uint64_t m_prefabPublication{};
        std::uint64_t m_prefabObservedDocumentEpoch{};
        std::optional<PrefabHierarchySources> m_prefabSources;
        CancellationSource m_prefabCancellation;

        Assets::AssetRegistry *m_mutableAssetRegistry{};
        CollisionInventoryEvidence *m_collisionInventory{};
        CollisionActivationEvidence *m_collisionActivation{};
        const EditorPreviewCollisionReceiptSource *m_previewCollisionReceipt{};
        Assets::AssetId m_collisionSceneOwner;
        std::uint64_t m_collisionSceneGeneration{};
        ProjectMutationCoordinator *m_mutations{};
        DurableFileSystem *m_durableFiles{};
        std::shared_ptr<const Assets::IAssetRegistrySource> m_assetRegistrySource;
        const Assets::AssetImporterCatalogSnapshot *m_importerCatalog{};
        std::unique_ptr<Assets::AssetPreviewService> m_assetPreviews;
        SourceOpenNavigator m_sourceOpenNavigator;
        DiagnosticSourceNavigator m_diagnosticSourceNavigator;
        Application::GameplayBuildService *m_gameplayBuilds{};
        Application::GameplayBuildEnvironment m_gameplayBuildEnvironment;
        const ILocalizationService *m_localization{};
        std::optional<Application::GameplayBuildSessionId> m_gameplayBuildSession;
        bool m_playAfterGameplayBuild{false};
        float m_nativeBuildDebounceSeconds{-1.0F};
        bool m_nativeGameplayReloadPending{false};
        bool m_nativeGameplayReloadQuarantined{false};
        std::optional<std::filesystem::path> m_defaultScenePath;
        std::optional<SceneFileFingerprint> m_sceneFingerprint;
        std::optional<Error> m_initializationError;
        std::unique_ptr<SceneFileWatchService> m_sceneFileWatch;
        EditorWorkspaceViewModel m_viewModel;
        EditorDataBus m_dataBus;
        NotificationService m_notifications{m_dataBus};
        std::unique_ptr<EditorEngineEventBridge> m_engineEventBridge;
        /** @brief Refresh the sole borrowed Physics edit port after an actual document replacement. */
        void RefreshPhysicsInspectorPort();
        /** @brief Admit one selection-fenced Physics candidate through canonical history and events. */
        void ApplyPhysicsInspectorCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Check the actual selection authority and exact owner fence before candidate preparation. */
        [[nodiscard]] Result<SceneTransactionSnapshot> CapturePhysicsInspectorSnapshot(const EditorWorkspaceViewCommandData &command) const;
        Assets::AssetId m_physicsDocumentOwner;
        std::uint64_t m_physicsReplacementEpoch{};
        const std::thread::id m_physicsOwnerThread{std::this_thread::get_id()};
        SceneDocument m_document;
        EditorHistory m_history;
        std::optional<SceneDocumentTransactions> m_physicsTransactions;
        std::optional<SceneDocumentTransactions> m_collisionScene;
        std::shared_ptr<CollisionProjectSettingsSession> m_collisionSettings;
        std::unique_ptr<SceneDocumentTransactions> m_navigationScene;
        std::unique_ptr<NavigationProjectSettingsSession> m_navigationSettings;
        std::uint64_t m_navigationOpening{1};
        SceneDocumentCommandExecutor m_documentCommands{m_document, m_history};
        Assets::AssetId m_inspectorDocumentOwner;
        SceneDocumentTransactions m_inspectorDocument;
        std::uint64_t m_inspectorDocumentGeneration{};
        std::uint64_t m_inspectorBehaviorRegistryRevision{};
        std::uint64_t m_inspectorApprovalRevision{};
        bool m_inspectorAdmissionClosed{};
        Extensions::EditorInspectorHost *m_inspectorHost{};
        std::vector<InspectorProviderApproval> m_inspectorApprovals;
        CreateSceneObjectUseCase m_createSceneObject{m_document, m_documentCommands};
        InstantiateSceneAssetUseCase m_instantiateSceneAsset{m_document, m_documentCommands};
        EditorSelectionModel m_selection{m_document, m_dataBus};
        EditorViewportModel m_viewport{m_dataBus};
        Runtime::PrimitiveMeshCache m_primitiveMeshCache;
        EditorAssetMeshCache m_assetMeshCache;
        EditorViewportSceneSnapshot m_viewportScene;

        struct CameraPreviewState final {
            Cinematic::CinematicRuntimeService runtime;
            Runtime::CameraService camera;
            std::optional<Cinematic::CinematicCameraPlayback> playback;
            std::vector<Cinematic::SequenceFrameCameraCutRequest> crossings;
            std::uint64_t frame{};
            CameraPreviewState(Cinematic::CinematicRuntimeService &&runtimeValue, Runtime::CameraService &&cameraValue);
        };

        std::unique_ptr<CameraPreviewState> m_cameraPreview;
        std::uint64_t m_viewportSceneRevision{};
        bool m_assetPlacementPreviewActive{false};
        std::optional<SceneDocumentSnapshot> m_deferredRuntimeSnapshot;
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> m_gameplayRetirements{std::make_shared<Gameplay::GameModuleRetirementQueue>()};
        std::unique_ptr<ProjectGameplayRegistry> m_gameplayRegistry;
        std::unique_ptr<ProjectGameplayRegistry> m_pendingGameplayRegistry;
        EditorPlaySessionController m_playSession;
        std::string m_prePlayDocumentPanelId{"horo.viewport"};
        std::vector<std::filesystem::path> m_contentBrowserBackHistory;
        std::vector<std::filesystem::path> m_contentBrowserForwardHistory;
        bool m_contentBrowserRefreshPending{false};
        bool m_contentBrowserLoadingPresented{false};

        struct PendingContentBrowserPreview {
            std::string absolutePath;
            std::string contributionId;
            std::string providerVersion;
            Assets::AssetPreviewRequest request;
            std::optional<Assets::AssetPreviewHandle> handle;
        };

        std::vector<PendingContentBrowserPreview> m_pendingContentBrowserPreviews;
        std::vector<UiCanvasDocument> m_uiCanvasDocuments;
        float m_pcgAutosaveElapsedSeconds{};
        float m_pcgAutosaveRetrySeconds{};
        bool m_pcgAutosaveErrorPresented{};
        bool m_pcgDocumentsShutdown{};
        float m_autosaveElapsedSeconds{0.0F};
        float m_autosaveRetryDelaySeconds{0.0F};
        float m_sceneFileWatchElapsedSeconds{0.0F};
        float m_gameplaySourceWatchElapsedSeconds{0.0F};
        bool m_sceneFileWatchErrorPresented{false};
        DocumentStateId m_lastAutosavedState{};
        bool m_autosaveSuppressedForDiscard{false};
        DocumentRevision m_queuedRuntimeRevision{};
        Runtime::SceneDefinitionRevision m_activeRuntimeRevision{};
        Runtime::SceneDefinitionRevision m_queuedDefinitionRevision{};
        Runtime::SceneDefinitionId m_previewSceneId{1};

        struct ContentBrowserDeletePlan {
            std::filesystem::path projectRoot;
            std::filesystem::path source;
            std::vector<std::filesystem::path> sources;
            std::optional<Assets::AssetId> deletedAssetId;
            std::string deletedAssetProjectPath;
        };

        struct ContentBrowserRenamePlan {
            std::filesystem::path source;
            std::filesystem::path destination;
            std::vector<std::filesystem::path> sources;
        };

        struct ContentBrowserAssetTransferPlan {
            std::filesystem::path projectRoot;
            std::filesystem::path source;
            std::filesystem::path sourceSidecar;
            std::filesystem::path destination;
            std::vector<std::filesystem::path> companions;
            Assets::AssetId assetId;
        };

        using ContentBrowserPathMoves = std::vector<std::pair<std::filesystem::path, std::filesystem::path>>;

        [[nodiscard]] Result<Assets::AssetRegistryBuildReport> RebuildProjectAssetRegistry(const std::filesystem::path &root);
        void RefreshCollisionActivation();
        [[nodiscard]] Result<void> BindCollisionScene();
        void RebuildContentBrowserProjection(const std::filesystem::path &projectRoot, const std::filesystem::path &requestedDirectory);
        void ScheduleContentBrowserPreviews();
        void PollContentBrowserPreviews();
        [[nodiscard]] bool PollContentBrowserPreview(PendingContentBrowserPreview &pending);

        struct NativeGameplayReloadTransaction {
            enum class Phase : std::uint8_t {
                Begun,
                Retired,
                Activated,
                RolledBack,
                Committed,
                Degraded,
            };

            NativeGameplayRollbackArtifact rollbackArtifact;
            ProjectLuaGenerationSnapshot luaGeneration;
            EditorPlayReloadSnapshot playSnapshot;
            Gameplay::GameModuleReloadSnapshot moduleSnapshot;
            Phase phase{Phase::Begun};
        };

        [[nodiscard]] bool ProcessDocumentCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessPlayCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessSceneObjectCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessViewportPickCommand(const EditorWorkspaceViewCommandData &cmd);
        void HandleViewportPick(const ViewportPickRequest &request);
        void ApplyViewportPickSelection(const EditorViewportPickResult &picked, const ViewportPickRequest &request);
        [[nodiscard]] bool ProcessViewportCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Logs a rejected camera change or publishes its accepted projection to the workspace. */
        void ApplyViewportCameraChange(const Result<void> &result, const char *action);
        [[nodiscard]] bool ProcessViewportCameraCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessViewportEditCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessComponentCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessObjectPropertyCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessObjectComponentValueCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Routes typed audio authoring and preview intents on the workspace owner lane.
         * @param cmd Copied workspace intent. @return True when this is an audio command. */
        [[nodiscard]] bool ProcessAudioComponentCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessBehaviorCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessContentBrowserCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessContentBrowserNavigationCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessContentBrowserMutationCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessContentBrowserAssetCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessContentBrowserSourceCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessActivePanelCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessLayoutCommand(const EditorWorkspaceViewCommandData &cmd);
        void ReorderActivityBarItem(const EditorWorkspaceViewCommandData &cmd);
        void DockWorkspacePanel(const EditorWorkspaceViewCommandData &cmd);
        void ResizeWorkspacePanel(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool IsPanelActive(std::string_view panelId) const;
        void RemovePanelFromDock(std::string_view panelId, WorkspaceDockArea area);
        void ActivateBottomPanel(const EditorWorkspaceViewCommandData &cmd, std::vector<std::string> &displacedPanelIds);
        void ActivatePanelDock(WorkspaceDockArea area, const EditorWorkspaceViewCommandData &cmd,
                               std::vector<std::string> &displacedPanelIds);
        void SyncPanelHost(WorkspaceDockArea area, std::string_view panelId);
        void PublishPanelActivation(WorkspaceDockArea area, std::string_view panelId, bool panelWasActive,
                                    const std::vector<std::string> &displacedPanelIds);
        void MovePanelActivitySlot(const EditorWorkspaceViewCommandData &cmd);
        void RefreshSceneProjections();
        /** @brief Rebinds the sole borrowed transaction port at canonical Scene projection refresh with a never-reused host epoch. */
        void RefreshInspectorDocumentPort();
        /** @brief Retires approvals before an actual project behavior catalog publication; epochs never wrap. */
        void InvalidateInspectorBehaviorBindings();
        /** @brief Revalidates provider/selection/schema/current owner and submits exactly one existing atomic transaction. */
        [[nodiscard]] Result<SceneTransactionResult> ApplyInspectorBehaviorEdit(const InspectorExtensionEdit &request);
        /** @brief Admits only explicit current user approval for a resolved selected type and exact provider/declaration fields. */
        [[nodiscard]] Result<void> ApproveInspectorContribution(const InspectorExtensionEdit &request);
        /** @brief Routes explicit provider approval and typed edit gestures through the canonical owner lane. */
        bool ProcessInspectorContributionCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Prepares bounded overlap replacement and copied projection before atomically publishing explicit owner approval. */
        [[nodiscard]] Result<void> PublishInspectorApproval(const InspectorExtensionEdit &request,
                                                            const Extensions::EditorInspectorContribution &contribution);
        /** @brief Explicit source refresh; publishes a complete bounded cache or repairable typed placement failures. */
        void RefreshPrefabHierarchy(const SceneDocumentSnapshot &document);
        /** @brief Prepares all allocating source/row storage, then publishes one immutable owner generation.
         * @throws std::bad_alloc before publication; the outer refresh boundary preserves the prior cache. */
        void PrepareAndPublishPrefabHierarchy(const SceneDocumentSnapshot &document, std::uint64_t publication);
        /** @brief Routes exact source/member gestures after owner/session/revision and protected-boundary checks. */
        [[nodiscard]] bool ProcessPrefabHierarchyCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Retires cached source and selection evidence after a successful scene opening replacement. */
        void RetirePrefabHierarchy() noexcept;
        /** @brief Routes typed source publication/close intent through workspace-owned document services. */
        bool ProcessSourceDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Routes document tabs to their owning lifecycle and publishes detached source snapshots. */
        bool ProcessWorkspaceDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Refreshes source presentation and dirty flags without disk I/O or document mutation. */
        void RefreshSourceDocumentProjection();

        /** @brief Writes to a selected destination, optionally preserving active document identity. */
        void SaveSceneToPath(const std::filesystem::path &absolutePath, bool copyOnly);
        /** @brief Completes default metadata and active identity only after a successful destination source save.
         * @param snapshot Exact saved Scene. @param fingerprint Actual destination identity. @param destination Validated path.
         */
        void CompleteSceneSaveAs(const SceneDocumentSnapshot &snapshot, const SceneFileFingerprint &fingerprint,
                                 const std::filesystem::path &destination);
        /** @brief Closes a clean navigation group before replacing Scene identity. @param operation Diagnostic action.
         * @return False if any unsaved owner prevents replacement; no owner is discarded.
         */
        [[nodiscard]] bool PrepareNavigationSceneReplacement(const char *operation);
        [[nodiscard]] std::optional<std::filesystem::path> ResolveSceneSaveDestination(const std::filesystem::path &absolutePath) const;
        void InitializeProjectState(const std::filesystem::path &absoluteProjectRoot);
        void InitializeWorkspaceLayout();
        void InitializeInitialScene(const std::filesystem::path &absoluteProjectRoot);
        [[nodiscard]] bool LoadInitialScene(const std::filesystem::path &absoluteProjectRoot);
        void CreateBootstrapScene();
        void InspectInitialSceneRecovery(const std::filesystem::path &absoluteProjectRoot);
        [[nodiscard]] bool CommitSceneSave(const SceneDocumentSnapshot &snapshot, const SceneFileFingerprint &fingerprint,
                                           const std::optional<std::filesystem::path> &newActivePath = std::nullopt);
        /** @brief Replaces the active session from the validated external canonical scene. */
        void ReloadExternalScene();
        /** @brief Restores validated recovery content into a new dirty document session. */
        void RestoreSceneRecovery();
        /** @brief Explicitly discards recovery state and suppresses leave-checkpoint autosave. */
        void DiscardSceneRecovery();
        /** @brief Attempts one recovery write when the active document has unsaved content. */
        void WriteAutosaveRecovery();
        void QueueRuntimeScene(SceneDocumentSnapshot snapshot);
        void StartPlaySession();
        void BeginPlaySession();
        void StartGameplayBuild(bool playWhenReady);
        void UpdateGameplayBuild(float elapsedSeconds);
        [[nodiscard]] Application::GameplayBuildRequest MakeGameplayBuildRequest() const;
        [[nodiscard]] bool HasNativeGameplaySources() const;
        void StopPlaySession();
        void RefreshPlayStateProjection();
        [[nodiscard]] EditorViewportCamera ResolvePlayViewportCamera(const Runtime::RuntimeSceneView &runtimeView) const;
        void ExtractPlayViewportScene();
        [[nodiscard]] Result<EditorViewportCamera> ResolveCameraCutPreview(Runtime::RuntimeSceneView scene);
        void HandleCreatePrimitive(Runtime::PrimitiveId primitive, std::optional<SceneObjectId> parent);
        [[nodiscard]] bool ApplyAssetViewportPlacement(const AssetSceneDropRequest &request, const Math::Aabb &localBounds,
                                                       Math::Transform &localTransform, bool publishFailure = true) const;
        void PreviewAssetPlacement(const AssetSceneDropRequest &request);
        void CancelAssetPlacementPreview();
        void HandleInstantiateAsset(const AssetSceneDropRequest &request);
        [[nodiscard]] const Assets::AssetRecord *ResolveAssetDropRecord(const AssetSceneDropRequest &request,
                                                                        bool publishFailure = true) const;
        void HandleInstantiatedAssetCommand(const Result<SceneCommandResult> &result);
        void LoadDocumentAssetMeshes();
        [[nodiscard]] std::string Localized(std::string_view key, std::string_view fallback) const;
        void HandleDuplicateObject(SceneObjectId object);
        void HandleDeleteObject(SceneObjectId object);
        void HandleDeleteSelectedObjects(const std::vector<SceneObjectId> &objects);
        [[nodiscard]] std::vector<SceneObjectId> CollectExistingDeleteObjects(const std::vector<SceneObjectId> &objects,
                                                                              std::string &singleName, std::size_t &requestedCount) const;
        void HandleDocumentCommandResult(const Result<SceneCommandResult> &result, const char *operation);
        void PreviewObjectTransform(SceneObjectId object, const Math::Transform &transform);
        void PreviewObjectTransforms(std::span<const SceneObjectTransformUpdate> updates);
        void CancelObjectTransformPreview();
        void PreviewLightComponent(SceneObjectId object, const Runtime::LightComponent &light);
        void CancelLightComponentPreview();
        void RefreshViewportLightProjection();
        void RefreshSelectionProjection();
        void NavigateContentBrowser(const std::filesystem::path &absoluteDirectory, bool recordHistory);
        void NavigateContentBrowserBack();
        void NavigateContentBrowserForward();
        void NavigateContentBrowserUp();
        void RenameContentBrowserEntry(const std::filesystem::path &absolutePath, std::string_view newName);
        [[nodiscard]] std::optional<ContentBrowserRenamePlan> PrepareContentBrowserRename(const std::filesystem::path &absolutePath,
                                                                                          std::string_view newName);
        [[nodiscard]] bool ResolveContentBrowserRenameSources(ContentBrowserRenamePlan &plan);
        void DeleteContentBrowserEntry(const std::filesystem::path &absolutePath);
        [[nodiscard]] std::optional<ContentBrowserDeletePlan> PrepareContentBrowserDelete(const std::filesystem::path &source);
        [[nodiscard]] std::optional<std::filesystem::path> CreateContentBrowserTrash(const ContentBrowserDeletePlan &plan);
        [[nodiscard]] std::optional<std::vector<std::pair<std::filesystem::path, std::filesystem::path>>> MoveContentBrowserEntriesToTrash(
            const ContentBrowserDeletePlan &plan, const std::filesystem::path &trashDirectory);
        [[nodiscard]] bool PublishContentBrowserDeletion(const ContentBrowserDeletePlan &plan, const std::filesystem::path &trashDirectory,
                                                         const std::vector<std::pair<std::filesystem::path, std::filesystem::path>> &moved);
        void DuplicateContentBrowserAsset(const std::filesystem::path &absolutePath);
        void SetContentBrowserClipboard(const std::filesystem::path &absolutePath, ContentBrowserClipboardMode mode);
        void PasteContentBrowserAsset(const std::filesystem::path &absoluteDirectory);
        void TransferContentBrowserAsset(const ContentBrowserAssetTransferRequest &request);
        void CreateContentBrowserFolder(const std::filesystem::path &absoluteDirectory, std::string_view name);
        void CreateGameplayBehavior(const CreateGameplayBehaviorRequest &request);
        void CreateGameplayBehaviorFiles(const CreateGameplayBehaviorRequest &request, bool nativeBehavior,
                                         const std::filesystem::path &projectRoot, const std::filesystem::path &directory);
        void WriteGameplayBehaviorSource(const CreateGameplayBehaviorRequest &request, bool nativeBehavior,
                                         const std::filesystem::path &projectRoot, const std::filesystem::path &directory);
        void RefreshGameplayRegistry();
        void RefreshAvailableBehaviorProjection();
        void ApplyPendingGameplayRegistry();
        void ApplyNativeGameplayReload();
        [[nodiscard]] Result<NativeGameplayReloadTransaction> BeginNativeGameplayReload(const std::filesystem::path &projectRoot) const;
        [[nodiscard]] Result<void> RetireNativeGameplayGeneration(NativeGameplayReloadTransaction &transaction);
        [[nodiscard]] Result<std::unique_ptr<ProjectGameplayRegistry>> TryActivateNativeGameplayGeneration(
            const std::filesystem::path &projectRoot, NativeGameplayReloadTransaction &transaction);
        void CommitNativeGameplayReload(std::unique_ptr<ProjectGameplayRegistry> generation, NativeGameplayReloadTransaction &transaction);
        void RollbackNativeGameplayReload(const std::filesystem::path &projectRoot, NativeGameplayReloadTransaction transaction,
                                          const Error &candidateError);
        void DegradeNativeGameplayReload(NativeGameplayReloadTransaction &transaction, Error error);
        void ReimportContentBrowserAsset(const std::filesystem::path &absolutePath);
        void RevealContentBrowserEntry(const std::filesystem::path &absolutePath);
        /** @brief Admits a sequence source into the persistent workspace document host. */
        void OpenEmbeddedPCGSource(const SourceOpenResult &result);
        /** @brief Opens the persistent prefab source through the shared document registry. */
        void OpenEmbeddedPrefabSource(const SourceOpenResult &result);
        /** @brief Retires inactive stages and publishes immutable source/runtime projections. */
        void RefreshPrefabDocuments();
        /** @brief Captures source closure/recovery and queues one isolated stage. */
        void PreparePrefabStage(PrefabDocumentSession &document);
        /** @brief Publishes the exact durable recovery candidate for an active prefab source. */
        void RefreshPrefabRecovery(const PrefabDocumentSnapshot &snapshot);
        /** @brief Advances bounded preparation and extracts only a newly ready generation. */
        void ExtractPrefabStage(PrefabDocumentSession &document);
        /** @brief Executes an admitted durable or history action on its sole source owner. */
        Result<void> ApplyPrefabDocumentAction(PrefabDocumentSession &document, const EditorWorkspaceViewCommandData &command);
        /** @brief Applies source/session-fenced prefab application intents. */
        bool ProcessPrefabDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Refuses dirty closes unless explicitly discarded, then closes the shared tab. */
        Result<void> ClosePrefabDocument(DocumentInstanceId instance, bool discard);
        void UpdatePCGAutosave(float elapsedSeconds, int intervalMinutes);
        void RefreshPCGDocumentProjection(DocumentInstanceId instance);
        void OpenEmbeddedSequenceSource(const SourceOpenResult &result);
        /** @brief Dispatches one command to the explicit owning workspace subsystem. */
        void DispatchWorkspaceCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Routes admitted authored documents directly to their persistent host. */
        bool OpenEmbeddedAuthoredDocument(const SourceOpenResult &result);
        /** @brief Handles transient sequence presentation and explicit document lifecycle commands. */
        bool ProcessSequenceDocumentCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Loads and focuses one validated embedded UI Canvas source result. */
        void OpenEmbeddedUiCanvasSource(const SourceOpenResult &result);
        void OpenSourceFile(const SourceOpenRequest &request);
        void OpenDiagnosticSource(const DiagnosticSourceRequest &source);
        [[nodiscard]] bool CopyContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                     const std::filesystem::path &absoluteDestinationDirectory);
        [[nodiscard]] std::optional<ContentBrowserAssetTransferPlan> PrepareContentBrowserAssetTransfer(
            const std::filesystem::path &absoluteSource, const std::filesystem::path &absoluteDestinationDirectory, bool moving);
        [[nodiscard]] bool ResolveContentBrowserAssetTransferMetadata(const std::filesystem::path &projectRoot,
                                                                      const std::filesystem::path &source, Assets::AssetId &assetId,
                                                                      std::vector<std::filesystem::path> &companions);
        [[nodiscard]] std::optional<std::vector<std::filesystem::path>> CopyContentBrowserCompanions(
            const std::filesystem::path &source, const std::filesystem::path &sourceSidecar, const std::filesystem::path &destination,
            const std::vector<std::filesystem::path> &companions, std::span<const std::byte> sidecarBytes);
        [[nodiscard]] bool PublishCopiedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                            const std::filesystem::path &destination, Assets::AssetId copiedId,
                                                            const std::vector<std::filesystem::path> &created);
        [[nodiscard]] bool RollbackContentBrowserMutation(const std::filesystem::path &projectRoot, bool rollbackComplete,
                                                          std::string_view failureKey);
        [[nodiscard]] bool MoveContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                     const std::filesystem::path &absoluteDestinationDirectory);
        [[nodiscard]] std::optional<ContentBrowserPathMoves> MoveContentBrowserCompanions(
            const std::filesystem::path &source, const std::filesystem::path &destination,
            const std::vector<std::filesystem::path> &companions, std::string_view failureKey);
        [[nodiscard]] bool PublishMovedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                           const std::filesystem::path &destination, Assets::AssetId originalId,
                                                           const ContentBrowserPathMoves &moved);
        [[nodiscard]] bool PublishRenamedContentBrowserEntries(const ContentBrowserPathMoves &moved);
        void ClearContentBrowserClipboard() noexcept;
        void SetContentBrowserRollbackError(bool rollbackComplete, std::string_view failureKey);
        void RefreshContentBrowserAfterMutation();
        void RequestContentBrowserRefresh();
        void ReconcileContentBrowserNavigation();
        IAudioComponentPreview *m_audioPreview{};
        Assets::AssetId m_audioPreviewWorkspace;
    };
}  // namespace Horo::Editor
