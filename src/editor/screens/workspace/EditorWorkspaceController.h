#pragma once
#include "EditorWorkspaceHostServices.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/WorldBakeApplication.h"
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
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Editor/WorldStreamingPreviewRuntime.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/project_model/EditorSelectionModel.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/PCGPreviewWorkspace.h"
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

namespace Horo::Platform {
    class IProjectReadFiles;
}

namespace Horo::Editor {
    class IEditorPlayXrHost;
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

    /** @brief Explicit host composition for prefab source capture and isolated authoring previews. */
    struct EditorWorkspacePrefabServices {
        PrefabHierarchySourceCapture sources;
        EditorPlaySceneFactory stageFactory;
    };

    /** @brief Host presentation routes for validated source opens and diagnostic locations; empty callbacks retain workspace defaults. */
    struct EditorWorkspaceSourceNavigation {
        SourceOpenNavigator sourceOpenNavigator{};
        DiagnosticSourceNavigator diagnosticSourceNavigator{};
    };

    /** @brief Explicit procedural source catalog, optional migrator and detached preview inputs. */
    struct EditorWorkspaceProceduralServices {
        const PCG::IPCGGraphSourceMigrator *migrator{};
        PCGEditorCatalog catalog{.document = {.revision = 1}};
        std::optional<PCGPreviewInputs> previewInputs;
    };

    /**
     * @brief Optional host-composed services; value groups preserve borrowed/shared lifetimes and defaults.
     * @details Private caller migration: asset registry/source/importer inputs belong to assets; canonical
     * world/source/session/streaming inputs belong to world; bake stores and captures belong to worldBake.
     * Hosts pass the same values through these groups. No lookup, registration or activation occurs here.
     * The shared jobs capability remains outside all three groups for PCG, previews, watching and bake.
     */
    struct EditorWorkspaceDependencies {
        EditorWorkspaceAssetServices assets{};
        EditorWorkspaceWorldServices world{};
        EditorWorkspaceWorldBakeServices worldBake{};
        UiInspectorResources *uiInspectorResources{}; /**< Actual host resource publisher, outliving the workspace. */
        EditorWorkspaceCollisionEvidence collision{};
        ProjectMutationCoordinator *mutations{};
        DurableFileSystem *durableFiles{};
        JobSystem *jobs{}; /**< Shared by PCG, asset previews, source watching and world bake. */
        EditorWorkspaceSourceNavigation sourceNavigation{};
        EditorWorkspaceGameplayServices gameplay{};
        const ILocalizationService *localization{};
        EngineDataBus *engineEvents{};
        Application::NetworkDebuggerService *networkDebugger{};
        Extensions::EditorInspectorHost *inspectorHost{}; /**< Borrowed host, outliving this workspace. */
        IAudioComponentPreview *audioPreview{};           /**< Explicit output capability; absent means unavailable. */
        EditorWorkspacePrefabServices prefab;             /**< Explicit authoring composition; never discovered by panels. */
        EditorWorkspaceProceduralServices procedural;
        std::optional<SceneSource::TriggerVolumeConversionContext> triggerBinding; /**< Immutable project capture; omitted in Null. */
    };

    class EditorWorkspaceController {  // NOSONAR(cpp:S1820, cpp:S1448) Authoritative workspace controller
    public:
        /** @brief Supplies the SAME workspace caches/document through a weak owner-lane fence. */
        [[nodiscard]] EditorXrSceneSourcePort XrSceneSource() const noexcept {
            return m_xrSceneSource.Port();
        }

        /** @brief Selects the actual application XR host; null preserves authored-camera desktop Play. */
        void SelectXrPlayHost(IEditorPlayXrHost *host) noexcept {
            m_selectedXrPlayHost = host;
            m_viewModel.playViewSource = host ? EditorPlayViewSource::SelectedXr : EditorPlayViewSource::AuthoredCamera;
        }

        EditorWorkspaceController(const std::filesystem::path &projectRoot, Runtime::RuntimeSceneService &runtimeScene,
                                  const Assets::AssetRegistrySnapshot &assetRegistry = {},
                                  const EditorWorkspaceDependencies &dependencies = {});
        ~EditorWorkspaceController();

        /** @brief Refreshes source tools from the explicitly composed canonical world; no world is synthesized. */
        void RefreshWorldSourceProjection();
        /** @brief Refreshes native streaming evidence and immutable overlay geometry from the canonical active view. */
        void RefreshWorldStreamingPreview();
        /** @brief Prepares an explicit project-local cooked index inspection mount without mutating its bytes or policy.
         * @param relativePath Root-contained world.index artifact. @param expected Exact active document fence.
         * @return Prepared native owner or typed failure preserving the current mount. */
        [[nodiscard]] Result<void> MountWorldStreamingPreview(std::string_view relativePath, WorldDocumentFence expected);
        /** @brief Unbinds the exact owned epoch before native shutdown; never revokes a foreign mount. */
        void StopWorldStreamingPreview() noexcept;
        /** @brief Advances a finite native planner quantum, commits ready replacements, and binds actual owner references. */
        void ServiceWorldStreamingPreview();
        /** @brief Executes explicit mount/unmount intents through the canonical document fence. */
        [[nodiscard]] bool ProcessWorldStreamingPreviewCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Binds an active host view to the composed canonical document after validating the exact immutable root.
         * @param binding Current root, exact global origin and monotonic view incarnation.
         * @return Typed rejection for stale roots/views; no hidden replacement or ownership transfer. */
        [[nodiscard]] Result<void> SetWorldViewportBinding(const WorldViewportBinding &binding);
        /** @brief Explicitly binds the canonical owner of the host's active world view; no new document authority is allocated.
         * @param canonicalOwner Borrowed owner; detach before its destruction. Switching owners requires explicit detach first.
         * @param binding Current immutable root/origin and monotonic active-view revision.
         * @return Typed stale/conflict/lifecycle rejection without changing the previous binding. */
        [[nodiscard]] Result<void> SetWorldViewportBinding(PartitionedWorldWorkspace &canonicalOwner, const WorldViewportBinding &binding);
        /** @brief Host must call before destroying the borrowed world owner; retained reader leases remain immutable and valid. */
        void DetachWorldSourceOwner() noexcept;
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
        [[nodiscard]] Result<void> SaveCollisionSettings();
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

        /** @brief Captures current canonical world state for all panels, dropping obsolete leases on close/replacement.
         * @param forceQuery Rebuilds a changed presentation filter even when the document fence is unchanged. */
        void RefreshWorldDocumentProjection(bool forceQuery = false);
        /** @brief Opens contained version-one source into the one world owner outside draw paths.
         * @param relativePath Portable project-relative .horoworld path. @param expected Expected active document, absent for first open.
         * @return Typed admission outcome; failed reads/decodes/replacement preserve document and focus. */
        [[nodiscard]] Result<void> OpenWorldCompositionSource(std::string_view relativePath,
                                                              std::optional<WorldDocumentFence> expected = std::nullopt);

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

        /** @brief Reports whether common workspace input belongs to a live PCG document.
         * @return True only when the active instance has a canonical graph projection. */
        [[nodiscard]] bool HasActivePCGDocument() const noexcept;

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
        /** @brief Replaces detached preview input and cancels all old overlays/work without changing graph or Scene.
         * @param inputs New immutable provider/schema/evaluation admission. @return Success or invalid input rejection. */
        [[nodiscard]] Result<void> ReplacePCGPreviewInputs(PCGPreviewInputs inputs);
        /** @brief Nonblocking owner-lane preview completion/invalidation sweep, independent of panel visibility/autosave. */
        void UpdatePCGPreviews();
        /** @brief Saves every dirty graph through its one document service. @return Success only for durable publications. */
        [[nodiscard]] Result<void> SavePCGDocuments();
        /** @brief Drains graph ownership after explicit save/discard/recovery policy.
         * @param discardDirty Explicit leave decision. @return Success or retryable incomplete ownership result. */
        [[nodiscard]] Result<void> ShutdownPCGDocuments(bool discardDirty = false);
        /** @brief Stops play and drains native module retirement before workspace destruction. @return Typed busy if callbacks remain. */
        [[nodiscard]] Result<void> ShutdownGameplayModules();

        /** @brief Queues actual bake/validation from the canonical world owner, never a filtered browser subset.
         * @param mode Bake or validate. @param expected Exact current source publication.
         * @return New operation identity or typed rejection preserving the current bake and document. */
        [[nodiscard]] Result<OperationId> StartWorldBake(Application::WorldBakeMode mode, const WorldDocumentFence &expected);
        /** @brief Polls application-owned work and refreshes immutable presentation without blocking. */
        void UpdateWorldBake();
        /** @brief Installs explicit complete project/schema/dependency capture from the host.
         * @param capture Owned verified inputs; the same project requires a strictly newer non-wrapping revision.
         * Publication fences invalidate older work.
         * @return Success or typed rejection retaining the previous capture and accepted operation. */
        [[nodiscard]] Result<void> SetWorldBakeProject(Application::WorldBakeProjectCapture capture);
        /** @brief Requests cancellation only for the exact operation captured by presentation. @param expected Captured operation.
         * @return Success or typed stale rejection retaining replacement work. */
        [[nodiscard]] Result<void> RequestWorldBakeCancellation(std::optional<OperationId> expected);
        /** @brief Routes typed world-bake commands outside rendering. @param command Captured user intent.
         * @return Whether this operation owns the command. */
        [[nodiscard]] bool ProcessWorldBakeCommand(const EditorWorkspaceViewCommandData &command);

        /** @brief Exposes the one editor authoring residency owner for explicit load/unload/pin commands.
         * @return Owner-thread capability; source bytes must be authorized and read by the host outside draw paths. */
        [[nodiscard]] PartitionedWorldWorkspace &WorldDocuments() noexcept {
            return m_worldDocuments;
        }

        /** @brief Queries immutable partitioned document state. @return Owner-thread capability. */
        [[nodiscard]] const PartitionedWorldWorkspace &WorldDocuments() const noexcept {
            return m_worldDocuments;
        }

        /** @brief Routes page-aware selection through the actual editor selection adapter.
         * @param objects Authored page-scoped addresses. @param primary Optional member. @param expected Exact publication.
         * @return Typed committed world lease or rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SelectWorldObjects(
            std::span<const WorldStreaming::WorldAuthoringObjectAddress> objects,
            const std::optional<WorldStreaming::WorldAuthoringObjectAddress> &primary, const WorldDocumentFence &expected) {
            return m_selection.SetWorldObjects(m_worldDocuments, objects, primary, expected);
        }

        /** @brief Executes one world-layer intent against the canonical residency/selection owner.
         * @param intent Captured typed action and exact document fence. @return Complete immutable result or typed atomic rejection.
         */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> ApplyWorldLayerIntent(const WorldLayerOutlinerIntent &intent);

        /** @brief Includes dirty loaded page content in the ordinary workspace leave guard. @return Whether unsaved content is owned. */
        [[nodiscard]] bool HasDirtyWorldDocuments() const {
            const auto snapshot = m_worldDocuments.Snapshot();
            return snapshot.HasValue() && snapshot.Value()->dirty;
        }

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

        /** @brief Captures an exact ordinary Scene subtree gesture before its filename modal opens.
         * @param root Selected ordinary object. @return Immutable revision/opening fence or typed rejection.
         */
        [[nodiscard]] Result<PrefabCreationIntent> CapturePrefabCreationIntent(SceneObjectId root) const;

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
        /** @brief Poll owned style preparation and refresh cached UI Canvas resource fences without I/O. */
        void UpdateUiCanvasResources();
        /** @brief Polls project Lua sources and applies compatible safe-point reload candidates. */
        void UpdateGameplaySources(float elapsedSeconds);
        /**
         * @brief Advances dirty-scene autosave scheduling from committed editor policy.
         * @param elapsedSeconds Owner-thread elapsed frame time.
         * @param intervalMinutes Zero disables autosave; positive values schedule recovery writes.
         */
        void UpdateAutosave(float elapsedSeconds, int intervalMinutes);
        /** @brief Advances canonical source autosave with the committed idle-delay preference.
         * @param elapsedSeconds Host update delta. @param delaySeconds Zero disables source saves.
         */
        void UpdateSourceAutosave(float elapsedSeconds, int delaySeconds);
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
        /** @brief Composes the explicit controller-owned operation from host-borrowed process capabilities.
         * @param dependencies Frozen workspace composition inputs. */
        void InitializeWorldBake(const EditorWorkspaceDependencies &dependencies);
        /** @brief Reserves the existing project mutation guard without waiting; replacements share only this owner's active lease.
         * @param request Captured source capabilities; in-memory-only work needs no writer reservation.
         * @return Retained application operation proof or typed busy/unavailable failure, without cancelling accepted work. */
        [[nodiscard]] Result<std::shared_ptr<const ProjectMutationLease>> ReserveWorldBakeSources(
            const Application::WorldBakeRequest &request);
        /** @brief Opens/focuses the exact registered persistent definition asset route. */
        void OpenNavigationDefinitionTab(Assets::AssetId asset);
        /** @brief Projects constant-time dirty/focus/close state without copying Scene roots per frame. */
        void UpdateNavigationDocumentTabs();
        /** @brief Routes typed viewport source commands through the composed document/history authority. */
        [[nodiscard]] bool ProcessWorldSourceCommand(const EditorWorkspaceViewCommandData &command);
        PartitionedWorldWorkspace *m_partitionedWorld{};
        std::uint64_t m_nextWorldSourceId{1};
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
        JobSystem *m_pcgPreviewJobs{};
        PCG::PCGNodeCatalogSnapshot m_pcgPreviewCatalog;
        std::optional<PCGPreviewInputs> m_pcgPreviewInputs;

        struct PCGPreviewOwner final {
            DocumentInstanceId document;
            std::unique_ptr<PCGPreviewSession> session;
        };

        std::vector<PCGPreviewOwner> m_pcgPreviews;
        std::uint64_t m_nextPCGPreviewWorld{1};
        PrefabHierarchySourceCapture m_prefabSourceCapture;
        Assets::AssetId m_workspaceSession;
        const WorldStreamingPreviewRuntimePolicy *m_streamingPreviewPolicy{};
        WorldStreamingPreviewHost *m_streamingPreview{};
        std::unique_ptr<WorldStreamingPreviewRuntime> m_streamingRuntime;
        std::unique_ptr<WorldStreamingPreviewRuntime> m_pendingStreamingRuntime;
        std::string m_pendingStreamingPath;
        std::shared_ptr<const Platform::IProjectReadFiles> m_worldSourceFiles;
        /** @brief Atomically rebinds a fully prepared native replacement and retires the prior exact owner. */
        [[nodiscard]] Result<void> CommitPendingWorldStreamingPreview();
        std::uint64_t m_prefabPublication{};
        std::uint64_t m_prefabObservedDocumentEpoch{};
        std::optional<PrefabHierarchySources> m_prefabSources;
        std::optional<Prefab::PrefabValueSchemaSnapshot> m_navigationPrefabSchemas;
        /** @brief Explicitly compose inert built-in navigation topology metadata once for source and edit resolution. */
        [[nodiscard]] Result<void> EnsureNavigationPrefabSchemas(const Prefab::PrefabLimitProfile &limits);
        /** @brief Prepare and commit one exact Prefab-member gesture with canonical runtime graph validation. */
        void ApplyPrefabNavigationInspector(const NavigationInspectorRequest &request);
        /** @brief Commit one admitted Prefab gesture while its fresh source capture and mutation lease stay pinned. */
        [[nodiscard]] Result<SceneCommandResult> CommitPrefabNavigationInspector(const NavigationInspectorRequest &request);
        CancellationSource m_prefabCancellation;

        Assets::AssetRegistry *m_mutableAssetRegistry{};
        UiInspectorResources *m_uiInspectorResources{};
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
        /** @brief Refreshes named committed collision profiles outside the frame draw path. */
        void RefreshPhysicsProfiles();
        /** @brief Projects registered Physics materials from the owner-pinned asset snapshot. */
        void RefreshPhysicsMaterials();
        /** @brief Admit one selection-fenced Physics candidate through canonical history and events. */
        /** @brief Admit one atomic navigation gesture through existing document/history owners. */
        void ApplyNavigationInspectorCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Admit the complete ordinary selection and publish one canonical Scene transaction. */
        void ApplySceneNavigationInspector(const NavigationInspectorRequest &request);
        /** @brief Capture descriptor publications outside Draw without inventing defaults in degraded projects. */
        void RefreshNavigationInspectorDefinitions();
        void ApplyPhysicsInspectorCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Admit collider child selection and atomic gizmo edits through this exact Scene/history owner. */
        void ApplyColliderViewportCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Validate stable collider sub-selection without changing the document or selection. */
        [[nodiscard]] Result<void> ValidateColliderViewportSelection(std::span<const ColliderEditTarget> selection) const;
        /** @brief Commit an admitted collider gesture atomically and publish one canonical document event. */
        void CommitColliderViewportEdits(const ColliderViewportRequest &request, const SceneTransactionSnapshot &snapshot);
        /** @brief Check the actual selection authority and exact owner fence before candidate preparation. */
        [[nodiscard]] Result<SceneTransactionSnapshot> CapturePhysicsInspectorSnapshot(const EditorWorkspaceViewCommandData &command) const;
        Assets::AssetId m_physicsDocumentOwner;
        std::uint64_t m_physicsReplacementEpoch{};
        const std::thread::id m_physicsOwnerThread{std::this_thread::get_id()};
        PartitionedWorldWorkspace m_worldDocuments;
        std::unique_ptr<Application::WorldBakeApplication> m_worldBake;
        Application::WorldBakeProjectCapture m_worldBakeProject;
        OperationStore *m_worldBakeOperations{};
        std::weak_ptr<const ProjectMutationLease> m_worldBakeSourceReservation;
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
        EditorXrSceneSourceOwner m_xrSceneSource{m_document, m_primitiveMeshCache, m_assetMeshCache};
        IEditorPlayXrHost *m_selectedXrPlayHost{};
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

        /** @brief Disposable source-qualified gesture; never owns scene or history state or a project writer lease. */
        struct PrefabDropState final {
            AssetSceneDropFence fence;
            Assets::AssetId asset;
            Assets::AssetRegistryRevision registry;
            PrefabHierarchySources sources;
            PrefabDropGeometry geometry;
            Assets::AssetSourceIdentity projectMetadata;
        };

        std::optional<PrefabDropState> m_prefabDrop;

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
            std::uint64_t generation{};
            Assets::AssetPreviewRequest request;
            std::optional<Assets::AssetPreviewHandle> handle;
        };

        std::vector<PendingContentBrowserPreview> m_pendingContentBrowserPreviews;
        std::uint64_t m_contentBrowserPreviewGeneration{};
        std::size_t m_contentBrowserNextPreviewEntry{};
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
        /** @brief Refills bounded thumbnail admission from the immutable directory projection. */
        void AdmitContentBrowserPreviews();
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
        /** @brief Routes world presentation commands to the canonical world owner with exact revision fencing. */
        [[nodiscard]] bool ProcessWorldCompositionCommand(const EditorWorkspaceViewCommandData &command);
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
        /** @brief Admit and publish the canonical creation result before presentation callbacks. */
        [[nodiscard]] PrefabCreationOutcome ExecutePrefabCreation(const PrefabCreationIntent &intent);
        /** @brief Executes a confirmed create-from-selection through canonical document/source owners. */
        [[nodiscard]] bool ProcessCreatePrefabCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Routes exact source/member gestures after owner/session/revision and protected-boundary checks. */
        [[nodiscard]] bool ProcessPrefabHierarchyCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Retires cached source and selection evidence after a successful scene opening replacement. */
        void RetirePrefabHierarchy() noexcept;
        /** @brief Contains every postcommit observer failure without reverting authoritative Scene or asset state.
         * @param selectionChanged Whether the canonical selection owner committed a clear requiring notification.
         * @param observerFailure Engaged fallback optional prepared before publication and moved without throwing on observer failure.
         * @pre The immutable committed outcome and canonical selection are already published; observerFailure is engaged.
         * @post Every provider exception, including non-standard types, is contained and retained as presentation failure.
         */
        void NotifyPrefabCreation(bool selectionChanged, std::optional<Error> &observerFailure) noexcept;
        /** @brief Routes typed source publication/close intent through workspace-owned document services. */
        bool ProcessSourceDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Routes document tabs to their owning lifecycle and publishes detached source snapshots. */
        bool ProcessWorkspaceDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Refreshes source presentation and dirty flags without disk I/O or document mutation. */
        void RefreshSourceDocumentProjection();
        /** @brief Refreshes changed canvas projections outside GUI draw work. */
        void RefreshUiCanvasProjection();
        /** @brief Routes revision-fenced hierarchy and persistence operations to the canonical canvas owner. */
        bool ProcessUiCanvasCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Dispatch a validated canvas intent to its canonical owner or session preview capability. */
        [[nodiscard]] Result<void> ExecuteUiCanvasCommand(UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                                          const EditorWorkspaceViewCommandData &command,
                                                          const UiInspectorResourceSnapshot &resources);
        /** @brief Saves one canvas through the shared project mutation boundary and returns its typed outcome. */
        [[nodiscard]] Result<void> SaveUiCanvasSource(UiCanvasDocument &owner) const;
        /** @brief Joins explicit canvas close admission with workspace routing and presents dirty-state errors. */
        void CloseUiCanvasSource(DocumentInstanceId instance);
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
        /** @brief Tests exact source-gesture ownership and project mode admission without mutation. */
        [[nodiscard]] bool AdmitPrefabDrop(const AssetSceneDropRequest &request) const;
        /** @brief Acquires a short writer lease and verifies exact source/project/Scene publication evidence. */
        [[nodiscard]] Result<ProjectMutationLease> ValidatePrefabDropPublication(const AssetSceneDropRequest &request) const;
        /** @brief Prepares/reuses exact canonical source geometry; failure changes no authored owner. */
        [[nodiscard]] Result<void> PreparePrefabDrop(const AssetSceneDropRequest &request, const Assets::AssetRecord &record);
        /** @brief Revalidates source and scene fences, then executes one canonical typed semantic placement command. */
        void PlacePrefabAsset(const AssetSceneDropRequest &request, const Assets::AssetRecord &record);
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
        /** @brief Routes exact-frame canvas intents through canonical document commands. */
        bool ProcessPCGDocumentCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Translates common undo/redo/save intents only when the active document is PCG. */
        bool ProcessPCGDocumentShortcut(const EditorWorkspaceViewCommandData &command);
        /** @brief Validates one active-session fence and finite presentation before any edit. */
        Result<void> ApplyPCGEditorRequest(PCGWorkspaceDocument &document, const PCGEditorRequest &request);
        /** @brief Dispatches an admitted request to the one canonical document executor or workspace presentation. */
        Result<void> ExecutePCGEditorRequest(PCGWorkspaceDocument &document, const PCGEditorRequest &request,
                                             const PCGDocumentSnapshot &snapshot);
        /** @brief Captures the current canonical source into its isolated preview owner. */
        [[nodiscard]] Result<void> RequestPCGPreview(DocumentInstanceId instance, CancellationToken cancellation);
        /** @brief Checks the canonical catalog's explicit preview grant without granting or composing capabilities. */
        [[nodiscard]] bool HasPCGPreviewGrant() const noexcept;
        /** @brief Cancels one session and synchronously retires only on explicit canonical close/shutdown. */
        void RetirePCGPreview(DocumentInstanceId instance);
        void UpdatePCGAutosave(float elapsedSeconds, int intervalMinutes);
        void RefreshPCGDocumentProjection(DocumentInstanceId instance);
        void OpenEmbeddedSequenceSource(const SourceOpenResult &result);
        /** @brief Dispatches one command to the explicit owning workspace subsystem. */
        void DispatchWorkspaceCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Routes focused prefab persistence/history and global transport without granting Scene mutation. */
        void ProcessFocusedPrefabCommand(const EditorWorkspaceViewCommandData &command);
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
