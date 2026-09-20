#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Assets/AssetPreviewService.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/NotificationService.h"
#include "Horo/Editor/ProjectMutation.h"
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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor {
    class ILocalizationService;
    struct EditorViewportPickResult;
    /** @brief Host presentation adapter for one already-validated source-open result. */
    using SourceOpenNavigator = std::function<bool(const SourceOpenResult &)>;
    /** @brief Platform navigation capability for one validated diagnostic source location. */
    using DiagnosticSourceNavigator = std::function<bool(const DiagnosticSourceRequest &)>;

    /** @brief Optional services used by the workspace without transferring their ownership. */
    struct EditorWorkspaceDependencies {
        Assets::AssetRegistry *mutableAssetRegistry{};
        ProjectMutationCoordinator *mutations{};
        DurableFileSystem *durableFiles{};
        const Assets::AssetImporterCatalogSnapshot *importerCatalog{};
        JobSystem *jobs{};
        SourceOpenNavigator sourceOpenNavigator{};
        DiagnosticSourceNavigator diagnosticSourceNavigator{};
        Application::GameplayBuildService *gameplayBuilds{};
        Application::GameplayBuildEnvironment gameplayBuildEnvironment{};
        const ILocalizationService *localization{};
    };

    class EditorWorkspaceController {  // NOSONAR(cpp:S1820, cpp:S1448) Authoritative workspace controller
    public:
        EditorWorkspaceController(const std::filesystem::path &projectRoot, Runtime::RuntimeSceneService &runtimeScene,
                                  const Assets::AssetRegistrySnapshot &assetRegistry = {},
                                  const EditorWorkspaceDependencies &dependencies = {});
        ~EditorWorkspaceController() = default;

        [[nodiscard]] const EditorWorkspaceViewModel &ViewModel() const noexcept {
            return m_viewModel;
        }

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
            return m_viewportScene;
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
        Runtime::RuntimeSceneService &m_runtimeScene;
        Assets::AssetRegistrySnapshot m_assetRegistry;
        SourceFileOpenService m_sourceOpenService;
        Assets::AssetRegistry *m_mutableAssetRegistry{};
        ProjectMutationCoordinator *m_mutations{};
        DurableFileSystem *m_durableFiles{};
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
        SceneDocument m_document;
        EditorHistory m_history;
        SceneDocumentCommandExecutor m_documentCommands{m_document, m_history};
        CreateSceneObjectUseCase m_createSceneObject{m_document, m_documentCommands};
        InstantiateSceneAssetUseCase m_instantiateSceneAsset{m_document, m_documentCommands};
        EditorSelectionModel m_selection{m_document, m_dataBus};
        EditorViewportModel m_viewport{m_dataBus};
        Runtime::PrimitiveMeshCache m_primitiveMeshCache;
        EditorAssetMeshCache m_assetMeshCache;
        EditorViewportSceneSnapshot m_viewportScene;
        std::uint64_t m_viewportSceneRevision{};
        std::optional<SceneDocumentSnapshot> m_deferredRuntimeSnapshot;
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

        using ContentBrowserPathMoves = std::vector<std::pair<std::filesystem::path, std::filesystem::path>>;

        void RebuildContentBrowserProjection(const std::filesystem::path &projectRoot, const std::filesystem::path &requestedDirectory);
        void ScheduleContentBrowserPreviews();
        void PollContentBrowserPreviews();

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
        [[nodiscard]] bool ProcessViewportCameraCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessViewportEditCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessComponentCommand(const EditorWorkspaceViewCommandData &cmd);
        [[nodiscard]] bool ProcessObjectPropertyCommand(const EditorWorkspaceViewCommandData &cmd);
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
        /** @brief Atomically commits the current default-scene snapshot and updates dirty state on
         * success. */
        void SaveScene(bool overwriteConflict = false);
        /** @brief Writes to a selected destination, optionally preserving active document identity. */
        void SaveSceneToPath(const std::filesystem::path &absolutePath, bool copyOnly);
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
        void HandleCreatePrimitive(Runtime::PrimitiveId primitive, std::optional<SceneObjectId> parent);
        [[nodiscard]] bool ApplyAssetViewportPlacement(const AssetSceneDropRequest &request, const Math::Aabb &localBounds,
                                                       Math::Transform &localTransform) const;
        void HandleInstantiateAsset(const AssetSceneDropRequest &request);
        void LoadDocumentAssetMeshes();
        [[nodiscard]] std::string Localized(std::string_view key, std::string_view fallback) const;
        void HandleDuplicateObject(SceneObjectId object);
        void HandleDeleteObject(SceneObjectId object);
        void HandleDeleteSelectedObjects(const std::vector<SceneObjectId> &objects);
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
        void OpenSourceFile(const SourceOpenRequest &request);
        void OpenDiagnosticSource(const DiagnosticSourceRequest &source);
        [[nodiscard]] bool CopyContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                     const std::filesystem::path &absoluteDestinationDirectory);
        [[nodiscard]] std::optional<std::vector<std::filesystem::path>> CopyContentBrowserCompanions(
            const std::filesystem::path &source, const std::filesystem::path &sourceSidecar, const std::filesystem::path &destination,
            const std::vector<std::filesystem::path> &companions, std::span<const std::byte> sidecarBytes);
        [[nodiscard]] bool PublishCopiedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                            const std::filesystem::path &destination, Assets::AssetId copiedId,
                                                            const std::vector<std::filesystem::path> &created);
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
    };
}  // namespace Horo::Editor
