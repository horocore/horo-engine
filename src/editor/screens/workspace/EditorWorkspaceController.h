#pragma once

#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/GameplayTestService.h"
#include "Horo/Application/TerrainAuthoringAssets.h"
#include "Horo/Application/TerrainAuthoringCook.h"
#include "Horo/Application/WorldBakeApplication.h"
#include "Horo/Assets/AssetPreviewService.h"
#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorEngineEventBridge.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "Horo/Editor/NotificationService.h"
#include "Horo/Editor/PCGGraphDocumentService.h"
#include "Horo/Editor/PrefabProjectSettingsSession.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/SemanticDocumentHistory.h"
#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Editor/WorldStreamingPreviewRuntime.h"
#include "Horo/Foundation/Sha256.h"
#include "ai/queries/preview/EnvironmentQueryPreviewNavigation.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/language/ClangdProvider.h"
#include "editor/language/HoroScriptSyntax.h"
#include "editor/project_model/EditorSelectionModel.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/ai/decisions/DecisionGraphDocumentHistory.h"
#include "editor/screens/workspace/ai/queries/EnvironmentQueryWorkspaceSource.h"
#include "editor/screens/workspace/pcg/PCGPreviewWorkspace.h"
#include "editor/screens/workspace/prefab/assets/PrefabApplyTransaction.h"
#include "editor/screens/workspace/prefab/document/PrefabDocumentSession.h"
#include "editor/screens/workspace/prefab/hierarchy/PrefabHierarchySources.h"
#include "editor/source_editor/SourceReviewApplication.h"
#include "inspection/EditorDebugCapture.h"
#include "screen/EditorWorkspaceHostServices.h"
#include "terrain/assets/TerrainAssetCreation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo {
    class BuildOutputStore;
}

namespace Horo::Extensions {
    class EditorInspectorHost;
}

namespace Horo::Platform {
    class IProjectReadFiles;
}

namespace Horo::Application {
    struct ProjectAssetImportReceipt;
}  // namespace Horo::Application

namespace Horo::Editor {
    struct EditorSourceCommandEndpoint;
    struct EditorInspectionCommandEndpoint;
    struct ObjectInspectionRequest;
    struct TransformInspectionPage;
    struct BoundsInspectionPage;
    struct ComponentInspectionPage;
    struct GeometryInspectionPage;
    struct DistanceInspectionPage;
    struct GridInspectionPage;
    struct SurfaceInspectionPage;
    struct CollisionInspectionPage;
    struct PenetrationInspectionPage;
    struct VisualInspectionPage;
    struct EditorInspectionPhysics;
    struct InspectionSceneRevision;
    struct AgentSceneObjectRename;
    struct AgentSceneLightUpdate;
    struct EditorBuildCommandEndpoint;
    struct EditorTestCommandEndpoint;
    struct CapturedDecisionExternal;
    struct EnvironmentQueryPreviewPhysics;

    struct TerrainDocumentView;
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
        Application::GameplayTestService *tests{};
        Application::GameplayTestEnvironment testEnvironment{};
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements;
        std::shared_ptr<EditorPlaySaveRetirement> saveRetirement;
        std::shared_ptr<Extensions::ScriptModuleHostServices> moduleScripts;
        EditorPlaySceneFactory sceneFactory{};             /**< Host composition capturing the selected native and asset authorities. */
        const ConfigurationService *assistConfiguration{}; /**< Process authority outliving the workspace. */
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
        BuildOutputStore *diagnosticOutput{}; /**< Borrowed process output store, outliving the workspace. */
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
        const Application::TerrainAuthoringCookFactory *terrainCook{};
        const Application::TerrainAuthoringAssetFactory *terrainAssets{};
        AI::LatticeQueryAuthoringFactory latticeQueryFactory{};
        const AI::QuerySchemaRegistry *querySchema{}; /**< Application-owned immutable descriptor registry. */
        AI::QueryContextProviderAccess queryContextAccess;
        const EnvironmentQueryPreviewPhysics *queryPreviewPhysics{};
        const EnvironmentQueryPreviewNavigation *queryPreviewNavigation{};
        const EditorInspectionPhysics *inspectionPhysics{};
    };

    class IEditorViewportRenderer;

    class EditorWorkspaceController {  // NOSONAR(cpp:S1820, cpp:S1448) Authoritative workspace controller
    public:
        /** Explicit host operation on actual isolated Play; source must already be an authenticated immutable capture.
         * Project/context capability policy and both consents remain mandatory. Never runs package entry code automatically. */
        [[nodiscard]] Result<void> BeginModuleProgram(std::shared_ptr<const Extensions::ScriptModuleProgram>,
                                                      Extensions::ScriptModuleOperationPolicy, const CancellationToken & = {});
        [[nodiscard]] std::optional<Extensions::ScriptCallResult> TakeModuleProgramResult();
        [[nodiscard]] Result<void> RequestModuleReferenceUpdate(Extensions::ScriptExportVersion);
        [[nodiscard]] Result<Extensions::ScriptModuleUpdateReport> RequestNativeModuleUpdate(const Extensions::ScriptModuleProgram &);
        [[nodiscard]] std::optional<Extensions::ScriptModuleUpdateReport> TakeModuleUpdateReport();

        /** @brief Supplies the SAME workspace caches/document through a weak owner-lane fence. */
        [[nodiscard]] EditorXrSceneSourcePort XrSceneSource() const noexcept {
            return m_xrSceneSource.Port();
        }

        /** @brief Selects the mounted Play source and retires receipts from the previous draw owner. */
        void SelectMountedWorldPlay(bool selected) noexcept;

        /** @brief Selects the actual application XR host; null preserves authored-camera desktop Play. */
        void SelectXrPlayHost(IEditorPlayXrHost *host) noexcept {
            m_selectedXrPlayHost = host;
            m_viewModel.playViewSource = host ? EditorPlayViewSource::SelectedXr : EditorPlayViewSource::AuthoredCamera;
        }

        EditorWorkspaceController(const std::filesystem::path &projectRoot, Runtime::RuntimeSceneService &runtimeScene,
                                  const Assets::AssetRegistrySnapshot &assetRegistry = {},
                                  const EditorWorkspaceDependencies &dependencies = {});
        ~EditorWorkspaceController();

        /** @brief Binds a source presentation endpoint to the current editor workspace and authorization revision.
         * @param project Host-issued portable project identity. @param authorizationRevision Exact active MCP trust revision.
         * @return A borrowed lease suitable for the host's adapter composition.
         */
        [[nodiscard]] std::weak_ptr<const EditorSourceCommandEndpoint> BindSourcePresentation(std::string project,
                                                                                              std::uint64_t authorizationRevision);
        /** @brief Binds approved gameplay builds to this workspace's native build environment.
         * @param project Host-issued project identity. @param authorizationRevision Exact active trust revision.
         * @return Borrowed endpoint, or an empty lease when the project has no gameplay build capability.
         */
        [[nodiscard]] std::weak_ptr<const EditorBuildCommandEndpoint> BindGameplayBuildCommands(std::string project,
                                                                                                std::uint64_t authorizationRevision);
        /** @brief Binds the active workspace's configured native test operations.
         * @param project Host-issued project identity. @param authorizationRevision Exact active trust revision.
         * @return Borrowed endpoint, or an empty lease when the host lacks CTest capability.
         */
        [[nodiscard]] std::weak_ptr<const EditorTestCommandEndpoint> BindGameplayTestCommands(std::string project,
                                                                                              std::uint64_t authorizationRevision);
        /** @brief Revokes workspace command leases and cancels command-owned work before project retirement. */
        void RevokeWorkspaceCommands() noexcept;
        /** @brief Binds numeric inspection to this exact committed editor workspace. */
        [[nodiscard]] std::weak_ptr<const EditorInspectionCommandEndpoint> BindSceneInspection(std::string project,
                                                                                               std::uint64_t authorizationRevision);
        /** @brief Binds approved debug capture to this exact committed editor workspace.
         * @param project Host-issued project identity.
         * @param authorizationRevision Exact active trust revision.
         * @param viewport Optional renderer borrowed until the binding is revoked, replaced or the workspace retires.
         * @return A weak endpoint for the exact binding.
         */
        [[nodiscard]] std::weak_ptr<const EditorDebugCaptureEndpoint> BindDebugCapture(std::string project,
                                                                                       std::uint64_t authorizationRevision,
                                                                                       IEditorViewportRenderer *viewport = nullptr);

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
         * @param object Exact selected authored source. @param revision Expected document revision.
         * @param outputSwitch Explicit replacement intent, if supplied. @return Typed admission/failure. */
        [[nodiscard]] Result<void> StartAudioPreview(SceneObjectId object, DocumentRevision revision,
                                                     std::optional<AudioComponentPreviewOutputSwitch> outputSwitch = std::nullopt);
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

        /** @brief Starts a camera-only authored preview using the runtime camera admission policy.
         * @param activation Owned camera-only plan and player descriptor. @param cuts Required camera bindings/end policy.
         * @param view Host-issued generation-safe preview view. @param transitions Camera transitions admitted by the host.
         * @param crossFade Optional exact reservation from the selected viewport frontend.
         * @return Success or typed admission failure. */
        [[nodiscard]] Result<void> StartCameraCutPreview(
            Cinematic::SequencePlaybackActivation activation, const Cinematic::CameraCutActivation &cuts, Runtime::CameraViewContextId view,
            Runtime::CameraTransitionSupport transitions = Runtime::CameraTransitionSupport::SingleViewBlend,
            std::optional<Runtime::CameraCrossFadeAdmission> crossFade = std::nullopt);
        /** @brief Advances the preview with exact owner-clock ticks before viewport extraction.
         * @param delta Non-negative exact timeline delta. @return Success or typed runtime/binding failure. */
        [[nodiscard]] Result<void> AdvanceCameraCutPreview(Cinematic::SequenceTime delta);
        /** @brief Seeks without event traversal and immediately re-extracts the final camera.
         * @param position Exact authored timeline target. @return Success or typed seek/binding failure. */
        [[nodiscard]] Result<void> SeekCameraCutPreview(Cinematic::SequenceTime position);
        /** @brief Copies an effect sample into the active preview for the next viewport extraction.
         * @param view Exact current preview view. @param worldOffset Cosmetic world-space translation, or zero to clear.
         * @return Success or a typed stale preview or nonfinite sample rejection.
         */
        [[nodiscard]] Result<void> SetCameraCutPreviewShake(Runtime::CameraViewContextId view, Math::Vec3 worldOffset);
        /** @brief Copies the host's visual accessibility multiplier before preview extraction.
         * @param scale Validated scale in [0, 1].
         */
        void SetCameraShakeScale(float scale) noexcept;
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

        /** @brief Captures the ordinary Scene selection and its revocable lifetime on the owner lane.
         * @param purpose The immersive Play workflow may capture an empty selection in the active Play scene.
         * @return A scoped selection snapshot, or no value when the workflow is unavailable. */
        [[nodiscard]] std::optional<AgentSceneSelectionSnapshot> CaptureAgentSceneSelection(
            AgentSceneSelectionPurpose purpose = AgentSceneSelectionPurpose::SelectedObjects);
        /** @brief Reviews captured references against the same document and current Play scene. */
        [[nodiscard]] std::optional<AgentSceneReferenceReview> ReviewAgentSceneReferences(const AgentSceneReferenceRequest &request) const;
        /** @brief Reads the selected ordinary Scene object's latest complete Physics tick. */
        [[nodiscard]] std::optional<AgentSceneContactReview> ReadAgentSceneContacts();
        /** @brief Checks the exact document, Scene topology and selected owner of a retained preview. */
        [[nodiscard]] bool AgentSceneContactsCurrent(const AgentSceneContactReview &review) const;

        /** @brief Captures the currently authored camera and selection for one explicit local review. */
        [[nodiscard]] std::optional<AgentSceneSelectionSnapshot> CaptureAgentViewport(AgentViewportComponents components);

        /** @brief Checks a bounded set of proposed edit targets against the exact authored Scene on the owner lane.
         * @param revision Scene opening and revision of the proposal. @param objects Stable proposed object IDs.
         * @return Success when every target is editable, or the first typed rejection.
         */
        [[nodiscard]] Result<void> InspectAgentSceneEditTargets(InspectionSceneRevision revision,
                                                                std::span<const std::uint64_t> objects) const;

        /** @brief Displays a validated batch in the authored viewport while leaving the scene unchanged.
         * @param revision Exact scene opening and revision captured by the proposing client.
         * @param updates Bounded list of proposed object-local transforms.
         * @param renames Bounded list of proposed object names.
         * @param cancellation Revocable proposal lifetime.
         * @param authorized Optional live client authority for an embedded request.
         * @param lights Bounded list of enabled-state changes for existing authored lights.
         * @return Owner-issued review identity or a zero-mutation rejection.
         */
        [[nodiscard]] Result<std::uint64_t> PreviewAgentSceneEdits(InspectionSceneRevision revision,
                                                                   std::span<const SceneObjectTransformUpdate> updates,
                                                                   std::span<const AgentSceneObjectRename> renames,
                                                                   CancellationToken cancellation, std::function<bool()> authorized = {},
                                                                   std::span<const AgentSceneLightUpdate> lights = {});

        /** @brief Applies the displayed proposal after an explicit local approval.
         * @param review Exact owner-issued review identity.
         * @return Success or the scene admission error; the matched review is consumed.
         */
        [[nodiscard]] Result<void> ApproveAgentSceneReview(std::uint64_t review);

        /** @brief Cancels the displayed proposal only if its identity still matches.
         * @param review Exact owner-issued review identity.
         * @return Whether a matching review was retired.
         */
        bool CancelAgentSceneReview(std::uint64_t review);

        /** @brief Returns the captured native project root without reparsing presentation text.
         * @return Canonical/lexical root retained by this workspace owner. */
        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept {
            return m_projectRoot;
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
        /** @brief Captures authored decision revisions to fence delayed workspace departure. */
        [[nodiscard]] std::vector<std::pair<DocumentInstanceId, std::uint64_t>> DecisionGraphRevisions() const;
        /** @brief Capture terrain instances/revisions for exact delayed workspace leave decisions. */
        [[nodiscard]] std::vector<std::pair<DocumentInstanceId, std::uint64_t>> TerrainDocumentRevisions() const;
        /** @brief Settle asset-rooted terrain owners after explicit save/discard navigation. */
        [[nodiscard]] Result<void> ShutdownTerrainDocuments(bool discard = false);
        /** @brief Poll feature-owned cook transfers on the owner lane, without I/O or waiting. */
        void UpdateTerrainCook();
        /** @brief Explicit teardown only: close admission and drain accepted cook work before services retire. */
        void ShutdownTerrainCook() noexcept;
        /** @brief Capture actual bounded mesh/material registry choices for one transient resource creation modal. */
        [[nodiscard]] Result<std::vector<TerrainCreationAssetChoice>> CaptureTerrainAssetChoices() const;
        /** @brief Create one tracked resource through the explicitly composed factory and retain the immutable publication receipt. */
        [[nodiscard]] Result<std::shared_ptr<const Application::TerrainAuthoringAssetCreated>> CreateTerrainAsset(
            const TerrainDocumentView &document, const Application::TerrainAuthoringAssetRequest &request);

        /** @brief Last typed single-source save/Save As outcome from the actual workspace command route. */
        [[nodiscard]] const std::optional<Result<SourceSaveResult>> &SourceSaveOutcome() const noexcept {
            return m_sourceSaveOutcome;
        }

        /** @brief Last bounded Save All batch, including every independently failed document. */
        [[nodiscard]] const std::optional<Result<std::vector<SourceSaveAllItem>>> &SourceSaveAllOutcome() const noexcept {
            return m_sourceSaveAllOutcome;
        }

        /** @brief Reads source review feedback and stages an exact undo command through the workspace lane. */
        [[nodiscard]] const SourceReviewTransactions &SourceReviews() const noexcept {
            return m_sourceReviews;
        }

        /** @brief Projects current source review and native validation state for the attached panel. */
        [[nodiscard]] Proposals::SourceTransactionStatus ReadSourceReviewStatus() const;

        /** @brief Applies an explicit revision-fenced canvas close decision, preserving dirty state on failure. */
        [[nodiscard]] Result<bool> ResolveUiCanvasClose(UiCanvasDocumentStateId state, SourceCloseDecision decision);
        /** @brief Handles the shared close dialog's exact Blackboard revision choice. */
        [[nodiscard]] Result<bool> ResolveBlackboardClose(BlackboardDocumentFence fence, SourceCloseDecision decision);
        /** @brief Executes the explicit close decision only for its captured Decision document revision. */
        [[nodiscard]] Result<bool> ResolveDecisionGraphClose(DecisionGraphDocumentFence fence, SourceCloseDecision decision);
        /** @brief Clears this workspace's checkpoints for an explicit discard-and-leave action. */
        [[nodiscard]] Result<void> DiscardAiDocumentAutosaves();
        /** @brief Saves or discards one exact authored Query revision through the shared close modal. */
        [[nodiscard]] Result<bool> ResolveEnvironmentQueryClose(EnvironmentQueryDocumentFence fence, SourceCloseDecision decision);
        /** @brief Captures every editable Query revision for the shared workspace leave fence. */
        [[nodiscard]] std::vector<EnvironmentQueryDocumentFence> EnvironmentQueryRevisions() const;
        /** @brief Captures the current canvas instances and their exact publication fences for leave consent. */
        [[nodiscard]] std::vector<UiCanvasDocumentStateId> UiCanvasStates() const;
        /** @brief Retires all canvases after durable recovery or explicit discard. */
        [[nodiscard]] Result<void> ShutdownUiCanvasDocuments(bool discard = false);

        /** @brief Last explicit source close choice outcome. False retains the document after Cancel. */
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
        /** @brief Advances background recovery cadence without serializing or accessing native files. */
        void UpdatePrefabAutosave(float seconds, int minutes);
        /** @brief Drains queued recovery and flushes exact dirty prefab revisions for an explicit owner operation. */
        void FlushPrefabAutosave();
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
        /** @brief Returns only revocable owner-lane UI binding; the modal never retains this controller. */
        [[nodiscard]] std::weak_ptr<EditorPlayRuntimeBinding> PlaySaveBinding() const noexcept {
            return m_playSession.SaveBinding();
        }

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
        /** @brief Compose the exact project prefab document owner; no UI or runtime authority is created. */
        [[nodiscard]] Result<std::shared_ptr<PrefabProjectSettingsSession>> PrefabSettings();
        /** @brief Capture the accepted immutable profile for a new authoring/conversion operation. */
        [[nodiscard]] Result<Prefab::PrefabLimitProfile> PrefabLimits();
        /** @brief Observe accepted publication and invalidate authoring projections only at the host boundary. */
        void UpdatePrefabSettingsProjection();
        /** @brief Include settings edits in ordinary project save/leave semantics. */
        [[nodiscard]] bool HasDirtyPrefabSettings() const;
        /** @brief Save exact current settings draft before project leave. */
        [[nodiscard]] Result<void> SavePrefabSettings();
        /** @brief Close the exact opening after explicit save/discard policy. */
        [[nodiscard]] Result<void> ShutdownPrefabSettings(bool discard = false);
        /** @brief Read the draft fence captured by delayed leave consent; zero means no opening. */
        [[nodiscard]] std::uint64_t PrefabSettingsRevision() const;
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

        /** @brief Stages the existing activation command for a registered panel, honoring its saved dock placement. */
        void StagePanelActivation(std::string_view panelId, WorkspaceDockArea fallback, EditorWorkspaceViewCommandData &command) const;
        /** @brief Releases Play debugger control when the inspector was absent from the current draw. */
        void SetPlayDecisionInspectorVisibility(std::optional<AI::AgentHandle> agent);
        void UpdateFps(float fps);

        /** @brief Sets only the read-only package execution evidence accompanying the Decision catalog. */
        void SetDecisionPackageActivationTrustRequired(bool required) noexcept {
            m_viewModel.decisionPackageActivationTrustRequired = required;
        }

        /** @brief Projects an exact package catalog while the application owns its live activation.
         * @param source Owner-lane reacquisition callback, or empty to detach package declarations. */
        void SetPackageDecisionNodeSource(std::shared_ptr<const ProjectPackageDecisionNodeSource> source);

        /** @brief Replaces the Content Browser projection when a newer registry revision is published.
         */
        void RefreshAssets(const Assets::AssetRegistrySnapshot &assetRegistry);
        /** @brief Opens the containing directory of a current registered recording and returns its exact browser path. */
        [[nodiscard]] std::optional<std::string> RevealRecordedAsset(Assets::AssetId id);
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
        /** @brief Binds the host runner and trusted settings to this project's source owner. */
        void BindSourceLanguage(IExternalProcessRunner &runner, Language::ClangdSettings settings);
        /** @brief Pumps bounded language results on the workspace owner lane. */
        void UpdateSourceLanguage();
        /** @brief Publishes a terminal Project usage review only for its original document and registry revision. */
        void UpdateBlackboardUsageReview();
        /** @brief Polls one bounded read-only EQS job and invalidates evidence from a retired authored Scene. */
        void UpdateEnvironmentQueryPreview();
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
        /** @brief Refreshes the active preview against the cold-prepared viewport in the same extraction update.
         * @param capacity Observed viewport capacity, absent for compositions without a frontend observation.
         * @return Success or typed preview camera failure, with no replacement snapshot on a rejected camera. */
        [[nodiscard]] Result<void> SynchronizeRuntimeScenePreview(std::optional<EditorViewportCrossFadeCapacity> capacity);
        /** @brief Advances gameplay presentation callbacks without changing authoring state. */
        void UpdatePlayPresentation(float elapsedSeconds);

        /** @brief Forwards the active Play session's authenticated structural cutoff. */
        void UpdatePlaySceneCutoff(const Runtime::FrameContext &context);
        /** @brief Advances one play-mode fixed tick with already-routed semantic actions. */
        void UpdatePlayFixed(std::span<const Gameplay::GameplayInputAction> input, double fixedDeltaSeconds);
        [[nodiscard]] Result<void> BindPlayRuntimeDispatch(const Runtime::RuntimeDispatchSource &source);
        void UpdatePlayFixed(std::span<const Gameplay::GameplayInputAction> input, const Runtime::FixedStepContext &context);
        void PreparePlayFixedCommit(const Runtime::FixedStepContext &context);
        void CommitPlayFixedUpdate(const Runtime::FixedStepContext &context) noexcept;
        void AbortPlayFixedUpdate(const Runtime::FixedStepContext &context) noexcept;

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

        /** @brief Accepted disk identity and bounded authoring state for a single Decision session. */
        struct DecisionGraphDraft final {
            std::weak_ptr<const AI::DecisionBlackboardProviderMetadata> reviewedSubtreeCatalog;
            Sha256Digest savedDigest;
            Sha256Digest currentDigest;
            std::size_t acceptedBytes{};
            Assets::AssetSourceIdentity acceptedLayout;
            std::shared_ptr<const AI::DecisionGraphSource> savedGraph;
            std::shared_ptr<const DecisionGraphLayout> savedLayout;
            DecisionGraphDocumentHistory history;
            std::uint64_t recoveryRevision{};
            bool recoveryAttempted{};
            std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery;
        };

        std::map<DocumentInstanceId, DecisionGraphDraft> m_decisionGraphDrafts;
        float m_aiRecoverySeconds{};
        void UpdateAiDocumentRecovery(float elapsedSeconds, int intervalMinutes);
        void FlushAiDocumentRecovery();
        void InspectDecisionRecovery(DecisionGraphWorkspaceDocument &document);
        void ProcessDecisionRecovery(const EditorWorkspaceViewCommandData &command);
        void ProcessDecisionExternalChange(const DecisionExternalRequest &request);
        [[nodiscard]] Result<void> ResolveDecisionExternalChange(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                 const DecisionExternalRequest &request);
        [[nodiscard]] Result<void> AcceptInterruptedDecisionSave(DecisionGraphWorkspaceDocument &document,
                                                                 const CapturedDecisionExternal &captured);
        [[nodiscard]] Result<void> ApplyDecisionExternalChange(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                               DecisionExternalAction action, const CapturedDecisionExternal &captured);
        [[nodiscard]] Result<void> RestoreDecisionRecovery(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft);
        [[nodiscard]] Result<void> ValidateDecisionDraftRoot(const AI::DecisionGraphSource &graph,
                                                             DecisionGraphWorkspaceDocument &document) const;
        void DiscardDecisionRecoveryAfterSave(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft);
        [[nodiscard]] Result<void> DiscardOwnedDecisionRecovery(const DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft);
        [[nodiscard]] Result<void> WriteDecisionRecovery(const DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft);
        [[nodiscard]] Result<void> PublishDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft);
        /** @brief Validates the latest schema and publishes the canonical registered source under its native lease. */
        Result<void> SaveDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                            bool saveReferenceParticipant = false);
        /** @brief Joins an accepted recovery writer before its borrowed project services retire. */
        void JoinAiDocumentRecovery(std::optional<DocumentInstanceId> instance = {});
        /** @brief Observes terminal recovery and acknowledges the exact published revision. */
        void ObserveAiDocumentRecovery();
        void StartQueuedAiDocumentRecovery();

        struct AiDocumentRecoveryWork final {
            DocumentInstanceId instance;
            std::uint64_t revision;
            JobHandle job;
            std::shared_ptr<const DocumentRecoveryRecord> acceptedCheckpoint;
        };

        std::optional<AiDocumentRecoveryWork> m_aiDocumentRecovery;
        std::vector<DocumentInstanceId> m_aiRecoveryQueue;
        bool m_aiAutosaveDiscarded{};
        /** @brief Projects structured Decision findings into the workspace's Build Output. */
        void PublishDecisionGraphDiagnostics(const Error &error) const;
        /** @brief Obtains the project metadata generation for the supported authoring family. */
        [[nodiscard]] Result<std::shared_ptr<const AI::DecisionNodeGeneration>> AcquireDecisionDraftProviders(
            const AI::DecisionGraphSource &source) const;
        /** @brief Reviews one explicitly selected saved Behavior Tree under current project authority. */
        Result<void> ReviewDecisionSubtree(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                           const BehaviorTreeEditRequest &request);
        /** @brief Executes and commits one fully validated editable candidate. */
        template <typename Request>
        Result<void> ApplyDecisionGraphEdit(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft, const Request &request);
        /** @brief Applies one bounded layout candidate to the same revision and history timeline. */
        Result<void> ApplyDecisionGraphLayout(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                              const DecisionGraphLayoutRequest &request);
        /** @brief Replays the next heterogeneous Decision history entry. */
        Result<void> ReplayDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft, bool redo);
        /** @brief Captures one exact current Decision document for prepared publication. */
        [[nodiscard]] static DecisionGraphHistoryFence CaptureDecisionHistoryFence(const DecisionGraphWorkspaceDocument &document,
                                                                                   const DecisionGraphDraft &draft) noexcept;
        /** @brief Reserves reviewed key-retarget bytes for a clean session with the exact previously accepted source.
         * @param document Existing Decision participant. @param draft Its history and accepted source state.
         * @param revision Expected document revision. @param source Reviewed registered record and canonical replacement bytes.
         * @param group Shared nonzero Blackboard transaction identity.
         * @return Detached reservation, or the typed decoder, stale-source or capacity failure.
         * @pre The coordinator has validated the entire candidate closure against its paired schema.
         */
        [[nodiscard]] Result<DecisionGraphDocumentHistory::Prepared> PrepareDecisionReferenceChange(
            const DecisionGraphWorkspaceDocument &document, const DecisionGraphDraft &draft, std::uint64_t revision,
            const Assets::AssetCookPinnedSource &source, std::uint64_t group) const;
        /** @brief Publishes one fully prepared Decision root before the coordinated view refresh.
         * @param document Same participant session. @param draft Its authoring history. @param prepared Exact live reservation.
         * @return False if the owner no longer matches the reservation. No state is changed on failure.
         */
        [[nodiscard]] bool PublishDecisionGraphChange(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                      DecisionGraphDocumentHistory::Prepared &prepared) noexcept;
        /** @brief Publishes the semantic draft flags to shared tab lifecycle. */
        void ProjectDecisionGraphDraft(DecisionGraphWorkspaceDocument &document, const DecisionGraphDraft &draft);

        Assets::AssetRegistrySnapshot m_assetRegistry;
        DocumentIdentityRegistry m_documentRegistry;
        std::optional<Result<SourceSaveResult>> m_sourceSaveOutcome;
        std::optional<Result<std::vector<SourceSaveAllItem>>> m_sourceSaveAllOutcome;
        SourceReviewTransactions m_sourceReviews;
        std::optional<Result<bool>> m_sourceCloseOutcome;
        /** @brief Intercept only explicitly registered terrain assets; generic source routing remains unchanged. */
        bool TryOpenTerrainDocument(const SourceOpenRequest &request);
        bool ProcessTerrainDocumentCommand(const EditorWorkspaceViewCommandData &command);
        void RefreshTerrainDocuments();
        /** @brief Resolve one owner-thread transient palette/selection state for an existing document. */
        [[nodiscard]] std::shared_ptr<TerrainToolSelection> TerrainSelection(DocumentInstanceId instance);
        [[nodiscard]] Result<std::shared_ptr<TerrainToolSelection>> PrepareTerrainSelection(const TerrainDocumentHost &document) const;
        /** @brief Validate the exact typed workspace target before dispatching any source or transient command. */
        [[nodiscard]] Result<TerrainDocumentHost *> TerrainTarget(const TerrainToolCommand &intent);
        [[nodiscard]] Result<TerrainDocumentHost *> TerrainTarget(const TerrainDocumentView &view, bool requireFocus);
        /** @brief Dispatch source actions independently from bounded stroke/palette production. */
        [[nodiscard]] Result<void> ExecuteTerrainTool(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> ExecuteTerrainStroke(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> ExecuteTerrainDocumentAction(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> ExecuteTerrainPublication(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> ExecuteTerrainPalette(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> ExecuteTerrainCook(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> EnsureTerrainCook(const std::filesystem::path &root);
        [[nodiscard]] Result<void> StartTerrainCook(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        void CancelTerrainCook(DocumentInstanceId instance) noexcept;
        [[nodiscard]] bool TerrainCookFenceMatches() const;
        [[nodiscard]] Result<void> RefreshTerrainAssetCreation(const TerrainDocumentView &view,
                                                               const Application::TerrainAuthoringAssetCreated &created,
                                                               Application::TerrainAuthoringAssetKind kind);
        [[nodiscard]] Result<void> ExecuteTerrainFoliageStroke(TerrainDocumentHost &document, const TerrainToolCommand &intent,
                                                               TerrainEditOperationId operation);
        [[nodiscard]] Result<void> ExecuteTerrainSelection(TerrainDocumentHost &document, const TerrainToolCommand &intent);
        [[nodiscard]] Result<void> AdmitTerrainOpen() const;
        bool FocusTerrainDocument(std::string_view path);
        [[nodiscard]] Result<void> OpenTerrainDocument(Assets::AssetId asset, const std::filesystem::path &absolute,
                                                       const std::string &path);
        void ObserveTerrainDocument(const TerrainDocumentHost &document);
        void ActivateTerrainCamera(const TerrainDocumentHost &document);
        void PublishTerrainFocus(std::optional<DocumentInstanceId> terrain, bool wasTerrain);
        bool TryCloseTerrainCommand(const EditorWorkspaceViewCommandData &command);
        void ProcessTerrainToolIntent(const TerrainToolCommand &intent);
        void ProcessFocusedTerrainCommand(const EditorWorkspaceViewCommandData &command);
        [[nodiscard]] Result<void> SaveTerrainDocuments();
        [[nodiscard]] Result<void> CloseTerrainDocument(DocumentInstanceId instance, bool discard);
        std::vector<std::shared_ptr<TerrainDocumentHost>> m_terrainDocuments;
        std::vector<std::pair<DocumentInstanceId, std::shared_ptr<TerrainToolSelection>>> m_terrainToolSelections;
        std::optional<DocumentInstanceId> m_activeTerrainDocument;
        std::optional<EditorViewportCamera> m_terrainPriorCamera;
        std::uint64_t m_nextTerrainOperation{1};
        const Application::TerrainAuthoringCookFactory *m_terrainCookFactory{};
        const Application::TerrainAuthoringAssetFactory *m_terrainAssetFactory{};
        std::optional<Application::TerrainAuthoringCookSession> m_terrainCook;
        std::shared_ptr<Application::TerrainAuthoringCookCompletion> m_terrainCookReceipt;
        std::optional<Application::TerrainAuthoringCookFence> m_terrainCookFence;
        DocumentInstanceId m_terrainCookDocument;
        bool m_terrainCookAdoption{};
        Assets::AssetRegistryRevision m_terrainCookRegistry;
        std::filesystem::path m_projectRoot;
        SourceFileOpenService m_sourceOpenService;
        IExternalProcessRunner *m_sourceLanguageRunner{};
        Language::ClangdSettings m_clangdSettings;
        std::unique_ptr<Language::ProjectLanguageWorkspace> m_sourceLanguage;
        std::shared_ptr<Extensions::ScriptModuleHostServices> m_scriptLanguageHost;
        std::shared_ptr<const EditorSourceCommandEndpoint> m_sourceCommandEndpoint;
        std::shared_ptr<const EditorInspectionCommandEndpoint> m_inspectionCommandEndpoint;
        std::shared_ptr<const EditorDebugCaptureEndpoint> m_debugCaptureEndpoint;
        CancellationSource m_debugCaptureLifetime;
        EditorDebugCaptureStorage m_debugCapture;
        std::function<bool()> m_debugCaptureAuthorized;
        Runtime::SceneRuntimeId m_debugCaptureScene;
        std::optional<Runtime::EntityRef> m_debugCaptureEntity;
        SceneObjectId m_debugCaptureObject;

        /** @brief Admits one capture in an existing active Play scene. */
        [[nodiscard]] Result<std::uint64_t> StartDebugCapture(const EditorDebugCaptureRequest &request, std::function<bool()> authorized);
        /** @brief Captures pixels only while the exact approved authored viewport remains current. */
        [[nodiscard]] Result<std::uint64_t> StartDebugImage(IEditorViewportRenderer &viewport, EditorDebugImageSource expected,
                                                            EditorViewportImageRegion region, std::function<bool()> authorized);
        /** @brief Observes revision and extent evidence for one complete authored viewport. */
        [[nodiscard]] std::optional<EditorDebugImageSource> ReadDebugImageSource(const IEditorViewportRenderer &viewport) const;
        /** @brief Samples the committed Play transformation on the editor owner lane. */
        void UpdateDebugCapture();
        /** @brief Reads a numeric page without changing authored state, selection, or presentation. */
        [[nodiscard]] Result<TransformInspectionPage> InspectLocalTransforms(const ObjectInspectionRequest &request) const;
        /** @brief Reads the world bounds of exact authored objects, using only a matching published geometry snapshot. */
        [[nodiscard]] Result<BoundsInspectionPage> InspectWorldBounds(const ObjectInspectionRequest &request) const;
        /** @brief Reads authored component counts without exposing component payloads. */
        [[nodiscard]] Result<ComponentInspectionPage> InspectComponents(const ObjectInspectionRequest &request) const;
        /** @brief Reads committed viewport geometry counts without contacting the renderer. */
        [[nodiscard]] Result<GeometryInspectionPage> InspectGeometry(const ObjectInspectionRequest &request) const;
        /** @brief Measures committed world bounds and forward orientation relative to a visible reference object. */
        [[nodiscard]] Result<DistanceInspectionPage> InspectDistances(const ObjectInspectionRequest &request) const;
        /** @brief Measures the offset of committed object origins to a requested world grid. */
        [[nodiscard]] Result<GridInspectionPage> InspectGridOffsets(const ObjectInspectionRequest &request) const;

        /** @brief Samples committed viewport mesh surfaces with a bounded world-space segment. */
        [[nodiscard]] Result<SurfaceInspectionPage> InspectSurfaces(const ObjectInspectionRequest &request) const;

        /** @brief Inspects the authored collider shapes contributed by one visible object. */
        [[nodiscard]] Result<CollisionInspectionPage> InspectCollisions(const ObjectInspectionRequest &request) const;

        /** @brief Reads positive penetration for an exact visible authored body pair. */
        [[nodiscard]] Result<PenetrationInspectionPage> InspectPenetrations(const ObjectInspectionRequest &request) const;

        /** @brief Reports projected bounds clipping and overlap in an explicit viewport region. */
        [[nodiscard]] Result<VisualInspectionPage> InspectVisualIssues(const ObjectInspectionRequest &request) const;
        /** @brief Checks that the current extraction belongs to the committed authored Scene. */
        [[nodiscard]] bool HasCurrentInspectionGeometry(const Runtime::RuntimeSceneView &active) const noexcept;
        /** @brief Fences inspection against the authored Scene and its current editor projection. */
        [[nodiscard]] Result<InspectionSceneRevision> InspectSceneRevision() const;
        std::shared_ptr<const EditorBuildCommandEndpoint> m_buildCommandEndpoint;
        std::shared_ptr<const EditorTestCommandEndpoint> m_testCommandEndpoint;
        std::optional<Application::GameplayBuildSessionId> m_commandBuildSession;
        std::optional<OperationId> m_commandTestOperation;

        struct SourceValidationRun final {
            std::shared_ptr<const SavedSourceReview> evidence;
            Application::GameplayBuildRequest request;
            std::optional<Application::GameplayBuildSessionId> build;
            std::optional<OperationId> test;
            Proposals::SourceReviewValidation refused{};
        };

        std::optional<SourceValidationRun> m_sourceValidation;
        Extensions::ScriptCapabilityScope m_scriptLanguageProject{Extensions::ScriptCapabilityScopeKind::Project};
        std::uint64_t m_scriptLanguageApiRevision{};
        Language::HoroScriptSyntaxWorkspace m_scriptSyntax;

        struct PendingSourceLanguage final {
            SourceLanguageIntent intent;
            Language::ProjectLanguageRequest request;
            Language::ProjectLanguageWorkspace::Clock::time_point deadline;
        };

        std::optional<PendingSourceLanguage> m_pendingSourceLanguage;
        PCGGraphDocumentService m_pcgDocuments;
        std::vector<std::unique_ptr<PrefabDocumentSession>> m_prefabDocuments;
        std::optional<DocumentInstanceId> m_activePrefabDocument;
        EditorViewportSceneSnapshot m_prefabViewportScene;
        std::uint64_t m_nextPrefabRuntime{EditorFirstPrefabStage.value};
        float m_prefabAutosaveSeconds{};

        struct PrefabRecoveryWork final {
            JobHandle job;
            std::shared_ptr<const PrefabRecoveryCheckpoint> checkpoint;
            std::shared_ptr<std::shared_ptr<const DocumentRecoveryRecord>> persisted;
        };

        /** @brief Worker writes its detached result once, before the job publishes terminal state. The UI reads after that state. */
        struct PrefabExternalWatchWork final {
            JobHandle job;
            std::shared_ptr<const PrefabExternalInspection> request;
            std::shared_ptr<std::shared_ptr<const PrefabExternalComparison>> comparison;
        };

        std::optional<PrefabExternalWatchWork> m_prefabExternalWatch;
        float m_prefabExternalWatchSeconds{};
        std::size_t m_nextPrefabExternalWatch{};
        std::optional<PrefabRecoveryWork> m_prefabRecoveryWork;
        std::vector<DocumentInstanceId> m_prefabRecoveryQueue;
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
        bool m_prefabApplyRecoveryPendingOnDisk{};
        std::array<std::optional<PrefabApplyRecovery>, 2> m_prefabApplyRecovery;
        std::optional<Prefab::PrefabValueSchemaSnapshot> m_navigationPrefabSchemas;
        /** @brief Explicitly compose inert built-in navigation topology metadata once for source and edit resolution. */
        [[nodiscard]] Result<void> EnsureNavigationPrefabSchemas(const Prefab::PrefabLimitProfile &limits);
        /** @brief Prepare and commit one exact Prefab-member gesture with canonical runtime graph validation. */
        void ApplyPrefabNavigationInspector(const NavigationInspectorRequest &request);
        /** @brief Commit one admitted Prefab gesture while its fresh source capture and mutation lease stay pinned. */
        [[nodiscard]] Result<SceneCommandResult> CommitPrefabNavigationInspector(const NavigationInspectorRequest &request);
        CancellationSource m_prefabCancellation;

        Assets::AssetRegistry *m_mutableAssetRegistry{};
        AI::LatticeQueryAuthoringFactory m_latticeQueryFactory{};
        const AI::QuerySchemaRegistry *m_querySchema{};
        AI::QueryContextProviderAccess m_queryContextAccess;
        const EnvironmentQueryPreviewPhysics *m_queryPreviewPhysics{};
        const EnvironmentQueryPreviewNavigation *m_queryPreviewNavigation{};
        const EditorInspectionPhysics *m_inspectionPhysics{};
        UiInspectorResources *m_uiInspectorResources{};
        JobSystem *m_sourceRecoveryJobs{};

        /** @brief Detached result slot written by one worker before the JobHandle publishes its terminal state. */
        struct EnvironmentQueryPreviewWork final {
            DocumentInstanceId instance;
            JobHandle job;
            std::shared_ptr<std::shared_ptr<const AI::QueryDebugSnapshot>> result;
        };

        std::optional<EnvironmentQueryPreviewWork> m_environmentQueryPreviewWork;
        EnvironmentQueryDocumentFence m_playQueryDigestFence;
        Runtime::SceneRuntimeId m_playQueryDigestRuntime;
        std::optional<Sha256Digest> m_playQueryDigest;
        void RefreshLiveEnvironmentQueryPreview(std::optional<Runtime::EntityRef> owner);
        /** @brief Borrows the built-in catalog or retains the active gameplay image's inert authoring schema for this transaction. */
        [[nodiscard]] std::shared_ptr<const AI::QuerySchemaRegistry> CaptureEnvironmentQueryAuthoringSchema() const;

        /** @brief Immutable semantic root retained by both history and the active Query draft. */
        struct EnvironmentQueryState final {
            std::shared_ptr<const AI::EnvironmentQueryAsset> source;
            EditableEnvironmentQuerySettings settings;
            Sha256Digest digest;
            std::size_t bytes{};
        };

        struct EnvironmentQueryHistoryEntry final {
            EnvironmentQueryState before;
            EnvironmentQueryState after;
            std::size_t bytes{};
        };

        struct EnvironmentQueryDraft final {
            EnvironmentQueryState current;
            std::shared_ptr<const AI::EnvironmentQueryAsset> savedSource;
            Sha256Digest saved;
            Sha256Digest sidecar;
            std::size_t acceptedBytes{};
            SemanticDocumentHistory<EnvironmentQueryHistoryEntry> history{};
            std::uint64_t recoveryRevision{};
            bool recoveryAttempted{};
            std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery;
        };

        /** @brief Stages an exact Query recovery candidate for an explicit user decision. */
        void InspectEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document) const;
        /** @brief Installs an accepted compatible Query checkpoint as a dirty draft. */
        Result<void> RestoreEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft);
        /** @brief Publishes a durable registered Query source while owning the mutation lease. */
        Result<void> PublishEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft);
        /** @brief Rebases an exact conflicted Query draft onto validated current disk inputs. */
        Result<void> ResolveEnvironmentQueryExternal(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft,
                                                     const EnvironmentQueryEditRequest &request);
        /** @brief Removes only a Query checkpoint accepted or captured by this draft. */
        Result<void> ClearEnvironmentQueryRecovery(const EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft);
        /** @brief Applies an exact restore or discard action for the candidate the user reviewed. */
        Result<void> ResolveEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft,
                                                     const EnvironmentQueryEditRequest &request);
        /** @brief Schedules immutable Query checkpoint input on the shared workspace recovery lane. */
        void StartQueuedEnvironmentQueryRecovery(DocumentInstanceId instance);
        /** @brief Checkpoints the latest dirty Query revisions at a workspace lifecycle boundary. */
        void FlushEnvironmentQueryRecovery();
        std::map<DocumentInstanceId, EnvironmentQueryDraft> m_environmentQueryDrafts;
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
        BuildOutputStore *m_diagnosticOutput{};
        Application::GameplayBuildService *m_gameplayBuilds{};
        Application::GameplayBuildEnvironment m_gameplayBuildEnvironment;
        Application::GameplayTestService *m_gameplayTests{};
        Application::GameplayTestEnvironment m_gameplayTestEnvironment;
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
        std::optional<CancellationSource> m_agentSceneSelectionLifetime;
        std::optional<CancellationSource> m_agentViewportLifetime;
        std::uint64_t m_agentViewportRevision{};

        struct AgentSceneReview final {
            std::uint64_t id;
            CancellationSource lifetime;
            SceneTransactionContext context;
            SceneDocumentTransactions owner;
            SceneDocumentTransactions::ReplacementPreview preview;
            std::function<bool()> authorized;
            DocumentChangeKind kind;
        };

        std::optional<AgentSceneReview> m_agentSceneReview;
        std::uint64_t m_agentSceneReviewSerial{};
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
        SceneDocumentTransactions m_prefabSceneTransaction;
        std::optional<SceneDocumentTransactions> m_physicsTransactions;
        std::optional<SceneDocumentTransactions> m_collisionScene;
        std::shared_ptr<CollisionProjectSettingsSession> m_collisionSettings;
        std::unique_ptr<SceneDocumentTransactions> m_navigationScene;
        std::unique_ptr<NavigationProjectSettingsSession> m_navigationSettings;
        std::shared_ptr<PrefabProjectSettingsSession> m_prefabSettings;
        std::uint64_t m_prefabSettingsPublication{};
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
        bool m_mountedWorldPlaySelected{};
        EditorViewportSceneSnapshot m_viewportScene;

        struct CameraPreviewState final {
            Cinematic::CinematicRuntimeService runtime;
            Runtime::CameraService camera;
            std::optional<Cinematic::CinematicCameraPlayback> playback;
            std::vector<Cinematic::SequenceFrameCameraCutRequest> crossings;
            Math::Vec3 shakeOffset{};
            std::uint64_t frame{};
            CameraPreviewState(Cinematic::CinematicRuntimeService &&runtimeValue, Runtime::CameraService &&cameraValue);
        };

        std::unique_ptr<CameraPreviewState> m_cameraPreview;
        float m_cameraShakeScale{1.0F};
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
        std::shared_ptr<const ProjectPackageDecisionNodeSource> m_packageDecisionNodes;
        std::unique_ptr<ProjectGameplayRegistry> m_pendingGameplayRegistry;
        std::shared_ptr<EditorPlaySaveRetirement> m_playSaveRetirement;
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
        Sha256Digest m_contentBrowserPreviewSchemaRevision;
        std::size_t m_contentBrowserNextPreviewEntry{};
        std::vector<std::unique_ptr<BlackboardSchemaDocument>> m_blackboardDocuments;

        /** @brief Exact checkpoint acknowledgement and transfer authority for one Blackboard tab. */
        struct BlackboardRecoveryState final {
            std::uint64_t recoveryRevision{};
            bool recoveryAttempted{};
            std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery;
        };

        std::map<DocumentInstanceId, BlackboardRecoveryState> m_blackboardRecovery;
        /** @brief Projects a prior recovery candidate for an explicit Blackboard decision. */
        void InspectBlackboardRecovery(BlackboardWorkspaceDocument &view) const;
        /** @brief Runs an exact Blackboard restore or discard, keeping canonical bytes untouched. */
        Result<void> ResolveBlackboardRecovery(BlackboardSchemaDocument &document, const BlackboardWorkspaceRequest &request);
        /** @brief Queues an immutable Blackboard capture for the shared AI I/O lane. */
        void CheckpointBlackboard(DocumentInstanceId instance, bool synchronous);
        /** @brief Synchronously flushes dirty Blackboard checkpoints during workspace shutdown. */
        void FlushBlackboardRecovery();
        /** @brief Removes only the checkpoint owned or accepted by this Blackboard tab. */
        Result<void> ClearBlackboardRecovery(const BlackboardSchemaDocumentSnapshot &snapshot);

        struct BlackboardUsageWork final {
            JobHandle job;
            BlackboardDocumentFence fence;
            std::shared_ptr<std::shared_ptr<const Application::BlackboardUsageReview>> result;
            std::optional<BlackboardReferenceEditPreview> preview;
            std::shared_ptr<const AI::DecisionNodeGeneration> generation;
        };

        std::optional<BlackboardUsageWork> m_blackboardUsageWork;
        std::map<DocumentInstanceId, std::shared_ptr<const AI::DecisionNodeGeneration>> m_blackboardReviewGenerations;
        std::vector<UiCanvasDocument> m_uiCanvasDocuments;
        float m_uiCanvasAutosaveSeconds{};

        struct CanvasRecoveryWork final {
            JobHandle job;
            std::shared_ptr<const UiCanvasRecoveryCheckpoint> checkpoint;
        };

        std::optional<CanvasRecoveryWork> m_uiCanvasRecovery;
        std::vector<DocumentInstanceId> m_uiCanvasRecoveryQueue;
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
        /** @brief Revokes agent evidence before publishing a changed scene or selection projection. */
        void InvalidateAgentSceneSelection() noexcept;
        /** @brief Revokes only viewport-specific reviews after camera or tool changes. */
        void InvalidateAgentViewport() noexcept;
        /** @brief Withdraws renderer-owned capture before retiring the command binding and its telemetry. */
        void RetireDebugCaptureBinding() noexcept;
        /** @brief Withdraws captures of the prior viewport projection before advancing its revision. */
        void AdvanceViewportSceneRevision() noexcept;
        /** @brief Copies a scene revision and bounded selection under the caller's owner lease. */
        [[nodiscard]] AgentSceneSelectionSnapshot CaptureAgentSelectionValues(const CancellationSource &lifetime) const;
        /** @brief Borrows the running Play scene built from the current authoring state. */
        [[nodiscard]] std::optional<Runtime::RuntimeSceneView> ReadAgentReferenceScene() const;
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
        /** @brief Admits another prefab pair only when earlier failed cleanup evidence no longer needs recovery. */
        [[nodiscard]] bool CanPublishPrefabPair() const;
        /** @brief Forks the exact retained draft through the existing prefab pair authority. */
        [[nodiscard]] Result<void> SavePrefabAs(const PrefabDocumentSnapshot &draft, const std::filesystem::path &destination);
        /** @brief Executes a confirmed create-from-selection through canonical document/source owners. */
        [[nodiscard]] bool ProcessCreatePrefabCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Routes exact source/member gestures after owner/session/revision and protected-boundary checks. */
        [[nodiscard]] bool ProcessPrefabHierarchyCommand(const EditorWorkspaceViewCommandData &command);

        /** @brief Commits a validated inspector revert while the exact source bytes and project lease remain pinned. */
        [[nodiscard]] Result<SceneCommandResult> RevertPrefabHierarchyOverrides(const PrefabHierarchyCommand &request,
                                                                                const PrefabHierarchyEntry &entry);
        /** @brief Retires cached source and selection evidence after a successful scene opening replacement. */
        void RetirePrefabHierarchy() noexcept;
        /** @brief Borrows the one Prefab transaction port for the current Scene opening. */
        [[nodiscard]] Result<SceneTransactionContext> CapturePrefabSceneContext();
        /** @brief Applies the selected Scene overrides using the already open canonical Prefab source owner. */
        [[nodiscard]] Result<void> ApplyPrefabHierarchyOverrides(const PrefabHierarchyCommand &request, const PrefabHierarchyEntry &entry);
        /** @brief Settles unreachable paired history before reporting either document's current replay actions. */
        void ReconcilePrefabApplyHistory();
        /** @brief Routes a coordinated head from either document, or false when ordinary history owns the command. */
        [[nodiscard]] Result<bool> ReplayPrefabApply(bool redo, PrefabDocumentSession *source = nullptr);
        /** @brief Publishes the paired completion and retains any unresolved native rollback evidence. */
        [[nodiscard]] Result<void> CompletePrefabApply(PrefabApplyOutcome outcome, DocumentChangeKind kind);

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
        /** @brief Admits a language provider once its compiler metadata is available. */
        void PrepareSourceLanguage();
        /** @brief Loads the project's authored script-to-module bindings. */
        void ConfigureScriptLanguage(std::shared_ptr<Extensions::ScriptModuleHostServices> host);
        /** @brief Reads current inert module metadata for the source's registered project binding. */
        [[nodiscard]] Result<std::vector<Extensions::ScriptModuleApiMetadata>> CaptureScriptLanguageImports(
            const SourceDocumentSnapshot &source);
        /** @brief Clears displayed script metadata when the host replaces or withdraws a schema. */
        void UpdateScriptLanguageRevision();
        /** @brief Reads the current schema revision without recapturing module metadata. */
        [[nodiscard]] std::uint64_t ScriptAuthoringApiRevision() const noexcept;
        /** @brief Localizes the current script diagnostics and refuses results from a retired registry revision. */
        void PublishScriptDiagnostics(Language::LanguageResult publication);
        /** @brief Resolves a module and queries its metadata in the exact source revision. */
        [[nodiscard]] Result<Language::LanguageResult> QueryScriptLanguage(const SourceLanguageIntent &intent);
        /** @brief Publishes one explicit Lua completion or hover request. */
        void RequestScriptLanguage(const SourceLanguageIntent &intent);
        /** @brief Revalidates an accepted Lua completion against the current registered provider. */
        [[nodiscard]] bool ScriptCompletionCurrent(const SourceLanguageCompletions &result, std::size_t index);
        /** @brief Asks the synchronized provider for the active source feature. */
        void RequestSourceLanguage(const SourceLanguageIntent &intent);
        /** @brief Retires a request if its document is no longer active or its deadline passed. */
        void ExpireSourceLanguageRequest(Language::ProjectLanguageWorkspace::Clock::time_point now);
        /** @brief Releases language projections from replaced or closed source publications. */
        void RetireStaleSourceLanguageProjections();
        /** @brief Observes the active source's background syntax check. */
        void UpdateScriptSyntax();
        /** @brief Stores diagnostics for the exact source revision shared by both language adapters. */
        void PublishSourceLanguageDiagnostics(Language::LanguageResult result);
        /** @brief Consumes one matching language reply from the current request. */
        void AcceptSourceLanguage(Language::LanguageResult result);
        /** @brief Displays completion choices for their originating view and source lease. */
        void AcceptSourceCompletions(Language::LanguageResult &result, const SourceEditingView &view, bool fromScriptModule = false);
        /** @brief Displays a symbol description for its originating selection. */
        void AcceptSourceHover(Language::LanguageResult &result, const SourceEditingView &view, bool fromScriptModule = false);
        /** @brief Applies a selected completion using the source document's history authority. */
        void ApplySourceCompletion(const SourceCompletionIntent &intent);
        /** @brief Opens an in-project source location and publishes its selection. */
        [[nodiscard]] bool FocusSourceLocation(const Language::LanguageLocation &target);
        /** @brief Opens a reference selected from the active source publication. */
        void OpenSourceReference(const SourceReferenceIntent &intent);
        /** @brief Publishes the byte selection of a symbol from the active source. */
        void SelectSourceSymbol(const SourceSymbolIntent &intent);
        /** @brief Publishes the document symbols from a current provider reply. */
        void AcceptSourceSymbols(Language::LanguageResult &result);
        /** @brief Publishes current-document proposals for explicit user review. */
        void AcceptSourceCodeActions(Language::LanguageResult &result, const SourceEditingView &view);
        /** @brief Previews or applies one still-current proposal using the source history owner. */
        void ReviewSourceCodeAction(const SourceCodeActionIntent &intent);
        /** @brief Starts an isolated gameplay build and gives the native workspace its normal completion notification. */
        [[nodiscard]] Result<Application::GameplayBuildSessionId> StartApprovedGameplayBuild(const std::function<bool()> &stop);
        /** @brief Consumes a deferred, explicitly approved build or test for the supplied audit. */
        void RunSourceReviewValidation(const EditorWorkspaceViewCommandData &command);
        /**
         * @brief Starts an isolated build for a newly approved saved source review.
         * @param audit The exact audit of the approved application.
         * @param stop Returns true when the editor command lifetime has ended.
         */
        void StartSourceReviewBuild(const std::shared_ptr<const Proposals::SourceReviewAudit> &audit, const std::function<bool()> &stop);
        /**
         * @brief Starts separately approved tests for the current successful review build.
         * @param stop Returns true when the editor command lifetime has ended.
         * @pre The current review status permits test execution.
         */
        void StartSourceReviewTests(const std::function<bool()> &stop);
        /** @brief Cancels operations owned by the current review and retires its presentation. */
        void RevokeSourceReviewValidation() noexcept;
        /** @brief Cancels the exact test operation and expires the workspace's borrowed command endpoint. */
        void RevokeGameplayTestCommands() noexcept;
        /** @brief Opens embedded source and publishes a validated optional text location on the editor lane. */
        [[nodiscard]] Result<void> PresentSourceCommand(const SourceDocumentId &path, std::optional<Language::LanguageLocation> target,
                                                        const std::function<bool()> &stop);
        /** @brief Refreshes changed canvas projections outside GUI draw work. */
        void RefreshUiCanvasProjection();
        /** @brief Routes revision-fenced hierarchy and persistence operations to the canonical canvas owner. */
        bool ProcessUiCanvasCommand(const EditorWorkspaceViewCommandData &command);
        /** @brief Dispatch a validated canvas intent to its canonical owner or session preview capability. */
        [[nodiscard]] Result<void> ExecuteUiCanvasCommand(UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                                          const EditorWorkspaceViewCommandData &command,
                                                          const UiInspectorResourceSnapshot &resources);
        /** @brief Saves one canvas through the shared project mutation boundary and returns its typed outcome. */
        [[nodiscard]] Result<void> SaveUiCanvasSource(UiCanvasDocument &owner);
        /** @brief Joins explicit canvas close admission with workspace routing and presents dirty-state errors. */
        void CloseUiCanvasSource(DocumentInstanceId instance);
        /** @brief Refreshes a cached recovery choice only at an explicit cold persistence boundary. */
        void InspectUiCanvasRecovery(const UiCanvasDocument &owner, UiCanvasWorkspaceDocument &view);
        /** @brief Checks due canvas recovery on the existing workspace autosave cadence. */
        void UpdateUiCanvasAutosave(float seconds, int minutes);
        /** @brief Polls one pending checkpoint without waiting, and retires it only at a terminal state. */
        bool ObserveUiCanvasRecovery();
        /** @brief Cancels and joins accepted work before mutating or releasing its host authorities. */
        [[nodiscard]] Result<void> DrainUiCanvasRecovery(std::optional<DocumentInstanceId> instance = {});
        /** @brief Places an immutable checkpoint on the dedicated shared I/O lane. */
        void ScheduleUiCanvasRecovery(UiCanvasDocument &canvas);
        /** @brief Presents a recovery failure only on the still-live matching document. */
        void ReportUiCanvasRecoveryError(DocumentInstanceId instance, const Error &error);
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
        [[nodiscard]] bool CommitSceneSave(DocumentRevision revision, DocumentStateId state, SceneFileFingerprint fingerprint,
                                           const std::optional<std::filesystem::path> &newActivePath = std::nullopt,
                                           DocumentChangeKind kind = DocumentChangeKind::SaveStateChanged);
        /** @brief Closed owner intents; every deferred replacement retains its original authoring consent. */
        enum class PlaySceneReplacementKind {
            External,
            Recovery
        };

        /** @brief Captures authoring identity without retaining a controller callback or a mutable document borrow. */
        struct PlaySceneReplacementIntent final {
            PlaySceneReplacementKind kind;
            DocumentRevision revision;
            std::uint64_t replacementEpoch{};
            std::optional<std::filesystem::path> scenePath;
        };

        std::optional<PlaySceneReplacementIntent> m_playSceneReplacement;
        /** @brief Stops and drains actual Play ownership before a replacement can enter existing publication. */
        [[nodiscard]] bool DeferPlaySceneReplacement(PlaySceneReplacementKind kind);
        /** @brief Drops stale authoring consent and executes a still-current intent only after real Play settlement. */
        void ResumePlaySceneReplacement();
        /** @brief Compares exact revision/load epoch/path; edits while waiting require new explicit consent. */
        [[nodiscard]] bool PlaySceneReplacementMatches() const noexcept;
        /** @brief Releases a superseded registry only after the old Play runtime no longer borrows it. */
        void FinishStoppedGameplayRegistry();
        /** @brief Replaces the active session from the validated external canonical scene. */
        void ReloadExternalScene();
        /** @brief Reads and validates detached external values before any canonical document mutation. */
        [[nodiscard]] Result<LoadedProjectScene> ReadExternalSceneForReplacement() const;
        /** @brief Commits already validated values through existing collision/document owners. */
        [[nodiscard]] bool InstallExternalScene(LoadedProjectScene scene, bool preserveRecovery = false);
        /** @brief Restores validated recovery content into a new dirty document session. */
        void RestoreSceneRecovery();
        /** @brief Explicitly reconciles the exact startup pair presented to the confirming user. */
        void RestorePrefabApplyRecovery(const std::shared_ptr<const PrefabApplyRecoveryCandidate> &accepted);
        /** @brief Resets live Scene projections after canonical replacement and optionally preserves draft recovery. */
        void FinishExternalSceneReplacement(bool preserveRecovery);
        /** @brief Explicitly discards recovery state and suppresses leave-checkpoint autosave. */
        void DiscardSceneRecovery();
        /** @brief Attempts one recovery write when the active document has unsaved content. */
        void WriteAutosaveRecovery();
        void QueueRuntimeScene(SceneDocumentSnapshot snapshot);
        [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> BuildRuntimeSceneProjection(const SceneDocumentSnapshot &snapshot);
        void StartPlaySession();
        void BeginPlaySession();
        /** @brief Captures the complete current native save inventory cold; errors disable save without silently dropping owners. */
        void BindPlaySaveSources();
        void StartGameplayBuild(bool playWhenReady);
        void UpdateGameplayBuild(float elapsedSeconds);
        [[nodiscard]] Application::GameplayBuildRequest MakeGameplayBuildRequest() const;
        [[nodiscard]] bool HasNativeGameplaySources() const;
        void StopPlaySession();
        void RefreshPlayStateProjection();
        /** @brief Projects the latest exact runtime failure while its Play scene remains active. */
        void RefreshPlayDecisionProblem();
        /** @brief Resolves a reported entity to the unchanged authored clone source, when it exists. */
        [[nodiscard]] std::optional<SceneObjectId> ResolvePlayDecisionProblemObject(const EditorPlayDecisionProblem &problem) const;
        /** @brief Revalidates the visible problem and its entity before changing selection. */
        void NavigatePlayDecisionProblem(const PlayDecisionNavigationRequest &request);
        void NavigatePlayDecisionGraphProblem(const PlayDecisionGraphNavigationRequest &request);
        /** @brief Resolves the exact current Play problem behind a selected Build Output row. */
        void NavigatePlayDecisionDiagnostic(std::uint64_t sequence);
        std::uint64_t m_playDecisionGraphFocusSequence{};
        [[nodiscard]] EditorViewportCamera ResolvePlayViewportCamera(const Runtime::RuntimeSceneView &runtimeView) const;
        void ExtractPlayViewportScene();
        /** @brief Clear unavailable simulated evidence before publishing another viewport revision. */
        void ClearSimulatorViewport();
        /** @brief Extract the exact current Play Scene with an explicitly admitted camera. */
        void ExtractPlayViewportWithCamera(const Runtime::RuntimeSceneView &runtimeView, const EditorViewportCamera &camera);

        struct CameraPreviewFrame final {
            EditorViewportCamera destination;
            std::optional<EditorViewportCameraCrossFade> crossFade;
        };

        [[nodiscard]] Result<CameraPreviewFrame> ResolveCameraCutPreview(Runtime::RuntimeSceneView scene,
                                                                         std::optional<EditorViewportCrossFadeCapacity> capacity);
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
        /** @brief Retires a displayed agent proposal once its authored viewport or authorization becomes stale. */
        void ReconcileAgentSceneReview();
        /** @brief Checks the pending review against the current document and workspace authority. */
        [[nodiscard]] bool IsAgentSceneReviewCurrent(const AgentSceneReview &review) const;
        /** @brief Publishes the localized status for a withdrawn or outdated review. */
        void NotifyAgentSceneReviewWithdrawn();
        /** @brief Checks whether the current authored viewport can safely display or approve a review. */
        [[nodiscard]] bool CanReviewAgentSceneEdits() const;
        /** @brief Borrows a live authored object only while visible, editable and within workspace scope. */
        [[nodiscard]] Result<const SceneObjectSnapshot *> FindAgentSceneEditTarget(SceneObjectId id) const;
        /** @brief Captures one exact visible object's replacement without altering authored state. */
        [[nodiscard]] Result<SceneObjectReplacement> CaptureAgentSceneReplacement(SceneObjectId id) const;
        /** @brief Merge an additional typed field change into the captured replacement for this object. */
        [[nodiscard]] Result<SceneObjectReplacement *> FindOrCaptureAgentSceneReplacement(
            SceneObjectId id, std::vector<SceneObjectReplacement> &changes) const;
        /** @brief Stage existing light changes for the same authored transaction as transforms and renames. */
        [[nodiscard]] Result<void> StageAgentSceneLights(std::span<const AgentSceneLightUpdate> lights,
                                                         std::vector<SceneObjectReplacement> &changes) const;
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
        /** @brief Defers actual module replacement while the old Play save owner still pins accepted worker/delegate sources. */
        [[nodiscard]] bool CanApplyNativeGameplayReload();
        /** @brief Reacquires only still-admitted old-generation save sources after a failure before runtime quiescence.
         * @return True when the original runner remains active; this does not claim Save readiness if actual rebind fails. */
        [[nodiscard]] bool RebindSaveAfterUnchangedNativeFailure();
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
        /** @brief Acknowledges the completed immutable recovery checkpoint on the document owner thread. */
        bool ObservePrefabRecovery();
        /** @brief Cancels/joins borrowed persistence work before save, reload or source owner retirement. */
        [[nodiscard]] Result<void> DrainPrefabRecovery(std::optional<DocumentInstanceId> instance = {});
        /** @brief Hands the exact detached checkpoint to the existing shared recovery I/O lane. */
        void SchedulePrefabRecovery(PrefabDocumentSession &document);
        /** @brief Observes dirty open Prefabs fairly using one background inspection and owner-lane publication. */
        void UpdatePrefabExternalWatch(float seconds);
        /** @brief Projects one completed inspection without waiting for background work. */
        bool ObservePrefabExternalWatch();
        /** @brief Cancels/joins detached disk inspection before workspace retirement. */
        void StopPrefabExternalWatch();
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
        /** @brief Loads or focuses an exact registry-backed Blackboard schema through the embedded workspace. */
        void OpenEmbeddedBlackboardSource(const SourceOpenResult &result);
        /** @brief Opens an exact registered decision source independently of runtime admission. */
        void OpenEmbeddedDecisionGraphSource(const SourceOpenResult &result, bool reloadExternal = false);
        /** @brief Releases the matching read-only decision tab if open. */
        bool CloseDecisionGraphSource(DocumentInstanceId instance);

        /** @brief Opens one exact registered query source in the normal workspace. */
        void OpenEmbeddedEnvironmentQuerySource(const SourceOpenResult &result, bool reloadExternal = false);
        /** @brief Restages the exact registered Query before discarding a conflicted draft. */
        void ReloadExternalEnvironmentQuery(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDocumentFence fence);
        /** @brief Routes a Run or Cancel request for the currently active registered saved query. */
        bool ProcessEnvironmentQueryPreview(const EditorWorkspaceViewCommandData &command);
        /** @brief Routes one revision-fenced Query edit or history command to the active document. */
        bool ProcessEnvironmentQueryEdit(const EditorWorkspaceViewCommandData &command);
        /** @brief Commits one detached typed Query edit or replays one shared history entry. */
        Result<void> ApplyEnvironmentQueryEdit(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft,
                                               const EnvironmentQueryEditRequest &request);
        /** @brief Publishes canonical query bytes through the Project owner and tracked native receipt. */
        Result<void> SaveEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document, EnvironmentQueryDraft &draft);
        /** @brief Saves dirty Query documents through the shared workspace Save All route. */
        Result<void> SaveEnvironmentQueryDocuments();
        /** @brief Projects one accepted semantic root and retires any preview of its previous source. */
        void ProjectEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document, const EnvironmentQueryDraft &draft);
        /** @brief Captures exact authoring inputs and submits pure bounded lattice evaluation. */
        Result<void> StartEnvironmentQueryPreview(EnvironmentQueryWorkspaceDocument &document,
                                                  const EnvironmentQueryPreviewRequest &request);
        [[nodiscard]] Result<EnvironmentQueryPreviewNavigation::Evaluate> CaptureEnvironmentQueryNavigation(
            const Runtime::RuntimeSceneView &scene, SceneObjectId anchor, AI::QueryStageId stage);
        /** @brief Frames a retained point from the same saved query and active authoring Scene revision. */
        void FocusEnvironmentQueryCandidate(EnvironmentQueryWorkspaceDocument &document, const EnvironmentQueryPreviewRequest &request);
        /** @brief Cancels the worker and detaches its result before retiring the controller. */
        void StopEnvironmentQueryPreview();
        /** @brief Returns true for the same published editor preview Scene and selected query anchor. */
        [[nodiscard]] bool IsEnvironmentQueryPreviewCurrent(const EnvironmentQueryPreviewScene &pin,
                                                            const std::optional<Runtime::RuntimeSceneView> &current) const;

        /** @brief Closes the matching immutable query source tab if present. */
        bool CloseEnvironmentQuerySource(DocumentInstanceId instance);
        /** @brief Dispatches a revision-fenced Blackboard document request. */
        bool ProcessBlackboardDocumentCommand(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Submits one explicit review of a durable key without blocking the workspace draw path. */
        [[nodiscard]] Result<void> StartBlackboardUsageReview(const BlackboardWorkspaceRequest &request,
                                                              const BlackboardSchemaDocument &document);
        /** @brief Publishes an exact reviewed migration through the Blackboard and Decision document owners. */
        [[nodiscard]] Result<void> ApplyBlackboardReferenceEdit(const BlackboardWorkspaceRequest &request,
                                                                BlackboardSchemaDocument &document);
        /** @brief Requests a coordinated undo or redo from any participating document. */
        [[nodiscard]] Result<void> ReplayBlackboardReferenceEdit(std::uint64_t group, bool redo);
        /** @brief Finds the reviewed documents needed by a new migration without opening or replacing their sessions. */
        [[nodiscard]] bool CanApplyBlackboardReferenceEdit(const BlackboardWorkspaceDocument &view) const;
        /** @brief Identifies documents whose individual save would split a staged migration. */
        [[nodiscard]] bool HasPendingBlackboardReferenceEdit(DocumentInstanceId instance) const;
        /** @brief Identifies members of the retained group, including an available redo. */
        [[nodiscard]] bool InBlackboardReferenceEdit(DocumentInstanceId instance) const;
        /** @brief Persists migrated Decision files before the schema, which is rechecked under the project lease. */
        [[nodiscard]] Result<void> SaveBlackboardReferenceEdit(BlackboardSchemaDocument &document, std::uint64_t revision);
        /** @brief Retires a group once its source changes are saved or its unapplied redo is abandoned. */
        void RetireBlackboardReferenceEdit();

        /** @brief Tracks the participants in a workspace-local history transition. */
        struct BlackboardReferenceGroup final {
            std::uint64_t identity{};
            DocumentInstanceId schema;
            std::vector<DocumentInstanceId> decisions;
            std::shared_ptr<const Application::BlackboardUsageReview> review;
            std::shared_ptr<const AI::DecisionNodeGeneration> nodes;
            bool applied{true};
            bool saving{};
        };

        /** @brief Prepared root and history from one exact current Decision participant. */
        struct PreparedDecisionReference final {
            DecisionGraphWorkspaceDocument *document;
            DecisionGraphDraft *draft;
            DecisionGraphDocumentHistory::Prepared reservation;
        };

        /** @brief Checks every participant, then swaps all roots without allocating or notifying between swaps. */
        [[nodiscard]] bool PublishBlackboardReferenceEdit(BlackboardSchemaDocument &document,
                                                          BlackboardSchemaDocument::PreparedReferenceChange &schema,
                                                          std::vector<PreparedDecisionReference> &decisions);
        std::optional<BlackboardReferenceGroup> m_blackboardReferenceGroup;
        std::uint64_t m_nextBlackboardReferenceGroup{1};
        /** @brief Revokes and joins an outstanding Project review before borrowed owners leave scope. */
        void StopBlackboardUsageReview();
        /** @brief Looks up the existing authored session by tab identity. */
        [[nodiscard]] BlackboardSchemaDocument *FindBlackboardDocument(DocumentInstanceId instance) noexcept;
        /** @brief Saves a Blackboard through the same native source publication owner as Save All. */
        [[nodiscard]] Result<void> SaveBlackboardDocument(BlackboardSchemaDocument &document, std::uint64_t revision);
        /** @brief Updates immutable tab data and typed diagnostic after one authorized operation. */
        void RefreshBlackboardDocument(BlackboardSchemaDocument &document, const Result<void> &result);
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
        /** @brief Executes typed state-machine authoring against the active document revision. */
        bool ProcessDecisionGraphEdit(const EditorWorkspaceViewCommandData &cmd);
        /** @brief Reopens the exact conflicted Decision document after an explicit discard request. */
        void ReloadExternalDecisionGraph(DecisionGraphDocumentFence fence);
        /** @brief Resolves a focused dirty Decision draft against the reviewed registered disk version. */
        void ResolveExternalDecisionGraph(DecisionGraphDocumentFence fence, DecisionExternalAction action);
        /** @brief Resolves a current finding to a registered Blackboard document and selects its exact key. */
        void OpenDecisionDiagnosticKey(const DecisionDiagnosticKeyRequest &request);
        /** @brief Saves all dirty intrinsic decision documents through the registered source owner. */
        Result<void> SaveDecisionGraphDocuments();
        /** @brief Records an admitted AI source by its registered identity. */
        void RememberRecentAiAsset(const SourceOpenResult &result);
        /** @brief Reprojects recent identities against the current project registry publication. */
        void RefreshRecentAiAssets();
        /** @brief Resolves a recent registered identity and opens its current validated source. */
        void OpenRecentAiAsset(Assets::AssetId asset);
        void OpenDiagnosticSource(const DiagnosticSourceRequest &source);
        /** @brief Creates a registered EQS source from an explicitly selected existing query. */
        [[nodiscard]] bool CopyEnvironmentQuerySource(const ContentBrowserAssetTransferPlan &plan, Assets::AssetId newId);

        /** @brief Resolves a portable and currently unoccupied destination in the project's actual Assets root. */
        [[nodiscard]] std::optional<std::filesystem::path> PrepareInitialAiAssetDestination(std::string_view directory,
                                                                                            std::string_view name,
                                                                                            std::string_view extension,
                                                                                            std::size_t maximumNameBytes);
        /** @brief Validates the browser request and attempts to create an initial authored Blackboard source. */
        void CreateContentBrowserBlackboard(const BlackboardCreationRequest &request);
        /** @brief Validates the browser request and attempts to author an initial state machine. */
        void CreateContentBrowserStateMachine(const StateMachineCreationRequest &request);
        /** @brief Refreshes starter Tasks from the actually admitted native gameplay generation. */
        void RefreshDecisionTaskChoices();
        /** @brief Checks the authored filename and target folder for a native Task-based Decision source. */
        void CreateContentBrowserTaskDecision(const TaskDecisionCreationRequest &request);
        /** @brief Publishes an initial Behavior Tree or Utility source with a real selected native Task. */
        void PublishInitialTaskDecision(const std::filesystem::path &destination, const TaskDecisionCreationRequest &request,
                                        Assets::AssetId newId);
        /** @brief Admits and opens one provider-free State Machine bound to an exact saved schema. */
        void PublishInitialStateMachine(const std::filesystem::path &destination, const StateMachineCreationRequest &request,
                                        Assets::AssetId newId);
        /** @brief Validates, publishes and opens one initial Blackboard with an explicitly authored first key. */
        void PublishInitialBlackboard(const std::filesystem::path &destination, const BlackboardCreationRequest &request,
                                      Assets::AssetId newId);
        /** @brief Validates the browser request and attempts to create an initial authored EQS source. */
        void CreateContentBrowserEnvironmentQuery(const EnvironmentQueryCreationRequest &request);
        /** @brief Builds the selected native query shape before any durable publication. */
        [[nodiscard]] Result<AI::EnvironmentQueryAsset> CreateInitialEnvironmentQuerySource(const EnvironmentQueryCreationRequest &request,
                                                                                            AI::QueryId id, AI::QueryStageId stage,
                                                                                            AI::QueryResultSchemaId result) const;
        /** @brief Creates and opens one registered query using the installed typed provider's authoring factory. */
        void PublishInitialEnvironmentQuery(const std::filesystem::path &destination, const EnvironmentQueryCreationRequest &request,
                                            Assets::AssetId newId);
        /** @brief Resolves the authored publication outcome and refreshes the shared browser and registry projection. */
        [[nodiscard]] bool FinishAuthoredAiPublication(const Application::ProjectAssetImportReceipt &receipt, const Result<void> &outcome,
                                                       std::string_view failureKey);
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
        std::uint64_t m_playDecisionDiagnosticSequence{};
    };
}  // namespace Horo::Editor
