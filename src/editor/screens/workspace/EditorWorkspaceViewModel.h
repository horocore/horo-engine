#pragma once

#include "EditorPlayState.h"
#include "Horo/Editor/ActivityBarLayout.h"
#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Editor/NavigationInspector.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Editor/PhysicsInspector.h"
#include "Horo/Editor/SequenceDocument.h"
#include "Horo/Editor/SourceEditingCommands.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Editor/WorldStreamingPreview.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "editor/document/SceneDocument.h"
#include "editor/project_model/EditorTransformTool.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "editor/screens/workspace/ContentBrowserModel.h"
#include "editor/screens/workspace/GameplayBehaviorRequest.h"
#include "editor/screens/workspace/InspectorExtensionEdit.h"
#include "editor/screens/workspace/PCGWorkspace.h"
#include "editor/screens/workspace/WorldLayerOutliner.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"
#include "editor/screens/workspace/prefab/PrefabHierarchyCommand.h"
#include "editor/ui/UiCanvasPreview.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Horo::Editor {
    class TerrainDocumentHost;
    struct TerrainToolCommand;
    /** @brief Typed presentation kind projected from authored scene components. */
    enum class SceneObjectKind : std::uint8_t {
        Mesh,
        GameObject,
        Camera,
        Light,
        TriggerVolume,
        AudioSource,
    };

    /** @brief Read-only presentation projection of one authored scene object. */
    struct SceneObject {
        SceneObjectId id;
        std::optional<SceneObjectId> parent;
        std::string name;
        SceneObjectKind kind{SceneObjectKind::GameObject};
        Math::Transform localTransform;
        SceneObjectComponentSet components;
        SceneObjectEditorState editorState;
        bool effectivelyVisible{true};
        bool effectivelyLocked{false};
        bool hiddenByParent{false};
        bool lockedByParent{false};
    };

    /** @brief One world-space Light marker projected from the current viewport render snapshot. */
    struct ViewportLightPresentation {
        SceneObjectId object;
        Render::RenderLight light;
    };

    /** @brief Typed viewport intent, executed only by the controller against its composed world owner. */
    enum class WorldSourceToolAction : std::uint8_t {
        Create,
        Edit,
        Remove,
        Undo,
        Redo
    };

    struct WorldSourceToolCommand {
        WorldSourceToolAction action{};
        WorldSourceEdit edit;
        Math::WorldCoordinate64 globalOrigin;
        std::uint64_t viewRevision{};
    };

    /** @brief Closed prefab application intents; destructive decisions are explicit. */
    enum class PrefabDocumentAction {
        Save,
        Undo,
        Redo,
        Reload,
        DiscardAndReload,
        Recover,
        DiscardRecovery,
        DiscardAndClose
    };

    enum class EditorWorkspaceViewCommand {
        None,
        WorldSourceTool,
        WorldLayerAuthoring,
        OpenCreateWorldLayer,

        MountWorldStreamingPreview,
        UnmountWorldStreamingPreview,
        OpenWorldCompositionSource,
        QueryWorldComposition,
        CloseWorldComposition,
        BakeWorldCells,
        ValidateWorldCells,
        CancelWorldBake,
        PrefabDocument,
        FocusWorkspaceDocument,
        CloseWorkspaceDocument,
        MoveWorkspaceDocument,
        FocusSequenceDocument,
        CloseSequenceDocument,
        UpdateSequenceTimeline,
        EditPCGGraph,
        ReturnToWelcome,
        SourceDocumentInteraction,
        SaveSourceDocument,
        SaveSourceDocumentAs,
        SaveAllSourceDocuments,
        CloseSourceDocument,
        UiCanvasSelect,
        UiCanvasCreate,
        UiCanvasMove,
        UiCanvasSearch,
        UiCanvasUndo,
        UiCanvasRedo,
        UiCanvasSave,
        UiCanvasApplyPresentation,
        UiCanvasSelectTheme,
        UiCanvasUpdatePreview,
        UiCanvasCommitLayout,
        UiCanvasReload,
        SaveScene,
        SaveSceneAs,
        SaveSceneCopyAs,
        CompareExternalScene,
        ReloadExternalScene,
        OverwriteExternalScene,
        RestoreSceneRecovery,
        DiscardSceneRecovery,
        UndoScene,
        RedoScene,
        StartPlay,
        PausePlay,
        ResumePlay,
        StepPlay,
        StopPlay,
        CreatePrimitive,
        PreviewAssetPlacement,
        CancelAssetPlacementPreview,
        InstantiateAsset,
        DuplicateObject,
        DeleteObject,
        DeleteSelectedObjects,
        SelectObject,
        PrefabHierarchy,
        CreatePrefabFromSelection,
        PickViewport,
        NavigateViewport,
        AlignViewportToAxis,
        ChangeViewportProjection,
        FocusViewportSelection,
        ChangeTransformTool,
        ChangeTransformSpace,
        PreviewObjectTransform,
        CommitObjectTransform,
        CancelObjectTransformPreview,
        PreviewLightComponent,
        CancelLightComponentPreview,
        UpdateObjectName,
        UpdateCameraComponent,
        UpdateLightComponent,
        UpdateTriggerVolumeComponent,
        UpdatePhysicsComponents,
        UpdateNavigationComponents,
        SelectPhysicsColliders,
        CommitPhysicsColliders,
        UpdateAudioSourceComponent,
        UpdateCharacterComponent,
        StartAudioComponentPreview,
        StopAudioComponentPreview,
        UpdateAudioListenerComponent,
        UpdateObjectEditorState,
        AddComponentToObject,
        RemoveComponentFromObject,
        AttachBehaviorToObject,
        UpdateBehaviorOnObject,
        UpdateInspectorBehaviorField,
        ApproveInspectorContribution,
        RemoveBehaviorFromObject,
        NavigateContentBrowser,
        NavigateContentBrowserBack,
        NavigateContentBrowserForward,
        NavigateContentBrowserUp,
        RefreshContentBrowser,
        RenameContentBrowserEntry,
        DeleteContentBrowserEntry,
        DuplicateContentBrowserAsset,
        CopyContentBrowserAsset,
        CutContentBrowserAsset,
        PasteContentBrowserAsset,
        TransferContentBrowserAsset,
        CancelContentBrowserClipboard,
        CreateContentBrowserFolder,
        CreateLuaBehavior,
        CreateNativeBehavior,
        ReimportContentBrowserAsset,
        RevealContentBrowserEntry,
        OpenSourceFile,
        OpenDiagnosticSource,
        ChangeActivePanel,
        CloseWorkspacePanel,
        ReorderActivityBarItem,
        DockWorkspacePanel,
        ResizePanel,
        OpenPlayTopologyProfiles,
        TerrainTool,
    };

    enum class BottomDockMode {
        Full,
        Split,
    };

    enum class BottomDockSlot {
        Left,
        Right,
    };

    enum class SideDockMode {
        Full,
        Split,
    };

    enum class SideDockSlot {
        Top,
        Bottom,
    };

    struct WorkspacePanelDropTarget {
        std::string targetNodeId;
        WorkspacePanelHost::DropKind kind = WorkspacePanelHost::DropKind::TabCenter;
    };

    /** @brief Normalized viewport click and renderer clip-depth convention forwarded for scene picking. */
    struct ViewportPickRequest {
        float normalizedX{0.0F};
        float normalizedY{0.0F};
        float aspect{1.0F};
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::NegativeOneToOne};
        bool toggleSelection{false}; /**< Toggle the hit object instead of replacing the selection. */
    };

    /** @brief Complete stable object-selection replacement requested by a UI adapter. */
    struct ObjectSelectionRequest {
        std::vector<SceneObjectId> objects;
        std::optional<SceneObjectId> primary;
    };

    /** @brief Operation selected for a direct Content Browser asset transfer. */
    enum class ContentBrowserTransferMode : std::uint8_t {
        Copy,
        Move,
    };

    /** @brief Absolute source and destination carried by a Content Browser drag/drop request. */
    struct ContentBrowserAssetTransferRequest {
        std::string absoluteSourcePath;
        std::string absoluteDestinationDirectory;
        ContentBrowserTransferMode mode{ContentBrowserTransferMode::Move};
    };

    /** @brief Native absolute path, optionally line-addressed diagnostic source requested by an output panel. */
    struct DiagnosticSourceRequest {
        std::filesystem::path absolutePath;
        std::uint32_t line{};
        std::uint32_t column{};
    };

    /** @brief Workspace-owned source lease and separate transient presentation state. */
    struct SequenceWorkspaceDocument {
        std::shared_ptr<const SequenceDocument> source;
        SequenceTimelineState timeline;
    };

    /** @brief Workspace-only preview session and its correlated immutable layout result or typed diagnostic. */
    struct UiCanvasPreviewPresentation final {
        UiCanvasPreviewState state;
        std::optional<UiCanvasPreview> layout;
        std::optional<Error> diagnostic;
    };

    /** @brief Cached immutable hierarchy presentation; no source data is rebuilt during drawing. */
    struct UiCanvasWorkspaceDocument final {
        DocumentIdentity identity;
        UiCanvasDocumentStateId state;
        Runtime::Ui::UiElementId root;
        UiCanvasPreviewPresentation preview;
        Runtime::Ui::UiElementId selection;
        std::uint64_t selectionRevision{1};
        Assets::AssetRegistryRevision inspectorAssets;
        Runtime::Ui::RuntimeStyleGeneration inspectorStyles;
        std::uint64_t inspectorResourcePublication{};
        std::shared_ptr<const std::vector<UiInspectorTheme>> inspectorThemes;
        Runtime::Ui::RuntimeStyleAssetId inspectorTheme;
        bool inspectorStylePending{};
        Runtime::Ui::UiDocumentRevision documentRevision;
        std::optional<Runtime::Ui::UiDocumentElement> selectedElement;
        std::vector<UiCanvasHierarchyRow> rows;
        std::string search;
        std::optional<Error> diagnostic;
        bool canUndo{};
        bool canRedo{};
        bool dirty{};
    };

    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    /** @brief One detached Physics gesture, fenced to the exact projected owner state. */
    struct PhysicsInspectorRequest final {
        SceneTransactionContext context;
        std::uint64_t selectionRevision{};
        PhysicsInspectorValues values;
    };

    /** @brief Detached complete navigation gesture; host proves exact owner, descriptors, sources and selection before publication. */
    struct NavigationInspectorRequest final {
        SceneTransactionContext context;
        std::uint64_t selectionRevision{};
        std::vector<SceneObjectId> objects;
        std::optional<PrefabHierarchyTarget> prefab;
        std::shared_ptr<const PrefabHierarchyProjection> publication;
        std::vector<NavigationDefinitionSnapshot> definitions;
        std::vector<NavigationInspectorEdit> edits;
    };

    /** @brief Detached collider gesture tied to one exact document and collider-selection generation. */
    struct ColliderViewportRequest final {
        SceneTransactionContext context;
        std::uint64_t selectionRevision{};
        std::vector<ColliderEditTarget> selection;
        std::vector<ColliderTransformEdit> edits;
    };

    struct EditorWorkspaceViewCommandData {  // NOSONAR(cpp:S1820) Command payload variant container
        EditorWorkspaceViewCommand command = EditorWorkspaceViewCommand::None;
        std::optional<WorldBrowserQuery> worldBrowserQuery;
        std::optional<WorldDocumentFence> worldDocumentFence;
        std::optional<OperationId> worldBakeOperation; /**< Exact accepted operation targeted by a cancellation intent. */
        std::optional<WorldSourceToolCommand> worldSource;
        std::shared_ptr<const TerrainToolCommand> terrainTool;
        std::optional<WorldLayerOutlinerIntent> worldLayerIntent;
        std::optional<DocumentInstanceId> documentInstance;
        std::optional<DocumentInstanceId> documentMoveBefore;
        std::optional<PrefabDocumentFence> prefabFence;
        std::shared_ptr<const DocumentRecoveryRecord> prefabRecovery;
        std::optional<PrefabSourceEdit> prefabEdit;
        PrefabDocumentAction prefabAction{PrefabDocumentAction::Save};
        std::optional<SourceEditingCommand> sourceInteraction;
        bool sourceTextInputOwned{}; /**< Current-frame ownership, independent of whether text changed. */
        std::optional<SourceSaveRequest> sourceSave;
        SourceCloseDecision sourceCloseDecision{SourceCloseDecision::Cancel};
        UiCanvasDocumentStateId uiCanvasState;
        Runtime::Ui::UiElementId uiCanvasElement;
        Runtime::Ui::UiElementKind uiCanvasKind{Runtime::Ui::UiElementKind::Panel};
        std::optional<UiCanvasPreviewState> uiCanvasPreviewState;
        Runtime::Ui::UiLogicalPoint uiCanvasLayoutDelta;
        std::optional<UiCanvasMoveElement> uiCanvasMove;
        std::optional<UiCanvasSetPresentation> uiCanvasPresentation;
        std::uint64_t uiCanvasSelectionRevision{};
        Runtime::Ui::UiDocumentRevision uiCanvasDocumentRevision;
        Assets::AssetRegistryRevision uiCanvasInspectorAssets;
        Runtime::Ui::RuntimeStyleGeneration uiCanvasInspectorStyles;
        std::uint64_t uiCanvasInspectorResourcePublication{};
        Runtime::Ui::RuntimeStyleAssetId uiCanvasTheme;
        std::optional<SequenceTimelineState> sequenceTimeline;
        std::optional<PCGEditorRequest> pcgRequest;
        std::optional<EditorMenuInvocation> menuInvocation = std::nullopt;
        std::optional<ObjectSelectionRequest> objectSelection = std::nullopt;
        std::optional<PrefabHierarchyCommand> prefabHierarchy;
        std::optional<PrefabCreationIntent> prefabCreation;
        std::optional<int> targetIndex = std::nullopt;
        std::optional<SceneObjectId> objectPayload = std::nullopt;
        std::optional<Runtime::PrimitiveId> primitivePayload = std::nullopt;
        std::optional<Math::Transform> transformPayload = std::nullopt;
        std::optional<std::vector<SceneObjectTransformUpdate>> transformUpdates = std::nullopt;
        std::optional<ViewportPickRequest> viewportPickPayload = std::nullopt;
        std::optional<EditorViewportNavigationDelta> viewportNavigationPayload = std::nullopt;
        std::optional<EditorViewportAxisView> viewportAxisPayload = std::nullopt;
        std::optional<Runtime::CameraProjection> viewportProjectionPayload = std::nullopt;
        std::optional<Runtime::CameraComponent> cameraPayload = std::nullopt;
        std::optional<Runtime::LightComponent> lightPayload = std::nullopt;
        std::optional<Runtime::TriggerVolumeComponent> triggerVolumePayload = std::nullopt;
        std::optional<PhysicsInspectorRequest> physicsPayload;
        std::optional<NavigationInspectorRequest> navigationPayload;
        std::optional<ColliderViewportRequest> colliderPayload;
        std::optional<Runtime::AudioSourceComponent> audioSourcePayload = std::nullopt;
        std::optional<Runtime::CharacterControllerComponent> characterPayload = std::nullopt;
        std::optional<Runtime::AudioListenerComponent> audioListenerPayload;
        std::optional<DocumentRevision> audioPreviewRevision;
        std::optional<SceneObjectEditorState> editorStatePayload = std::nullopt;
        std::optional<ComponentType> componentTypePayload = std::nullopt;
        std::optional<Gameplay::BehaviorTypeId> behaviorTypePayload = std::nullopt;
        std::optional<Gameplay::BehaviorComponent> behaviorPayload = std::nullopt;
        std::optional<InspectorExtensionEdit> inspectorEdit;
        std::optional<Gameplay::BehaviorInstanceId> behaviorInstancePayload = std::nullopt;
        std::optional<EditorTransformTool> transformToolPayload = std::nullopt;
        std::optional<EditorTransformSpace> transformSpacePayload = std::nullopt;
        std::optional<std::string> stringPayload = std::nullopt;
        std::optional<std::string> secondaryStringPayload = std::nullopt;
        std::optional<float> floatPayload = std::nullopt;
        std::optional<WorkspaceLayoutSize> layoutPayload = std::nullopt;
        std::optional<ActivityBarSlot> activityBarSlot = std::nullopt;
        std::optional<BottomDockSlot> bottomDockSlot = std::nullopt;
        std::optional<SideDockSlot> sideDockSlot = std::nullopt;
        std::optional<WorkspacePanelDropTarget> workspaceDropTarget = std::nullopt;
        std::optional<ContentBrowserAssetTransferRequest> contentBrowserTransfer = std::nullopt;
        std::optional<SourceOpenRequest> sourceOpenRequest = std::nullopt;
        std::optional<DiagnosticSourceRequest> diagnosticSource = std::nullopt;
        std::optional<CreateGameplayBehaviorRequest> gameplayBehaviorRequest = std::nullopt;
        std::optional<AssetSceneDropRequest> assetSceneDrop = std::nullopt;
    };

    /** @brief Pending project-local Content Browser clipboard operation. */
    enum class ContentBrowserClipboardMode : std::uint8_t {
        None,
        Copy,
        Move,
    };

    /** @brief Read-only projection of the controller-owned Content Browser clipboard. */
    struct ContentBrowserClipboardState {
        ContentBrowserClipboardMode mode{ContentBrowserClipboardMode::None};
        std::string absoluteSourcePath;
    };

    struct NavigationDefinitionEditorState;

    struct NavigationDefinitionEditorTab final {
        DocumentIdentity identity;
        Assets::AssetId asset;
        std::shared_ptr<NavigationDefinitionEditorState> editor;
    };

    /** @brief Name and stable identity of a project collision profile available to the Inspector. */
    struct PhysicsProfileChoice {
        Physics::CollisionProfileId id;
        std::string name;
    };

    /** @brief Name and stable identity of a registered Physics material. */
    struct PhysicsMaterialChoice {
        Assets::AssetId id;
        std::string name;
    };

    struct EditorWorkspaceViewModel {  // NOSONAR(cpp:S1820) Comprehensive workspace presentation model
        WorldViewportBinding worldViewport;
        std::shared_ptr<const WorldStreamingPreviewGeometry> worldStreamingPreview;
        std::optional<Error> worldStreamingPreviewError;
        std::optional<Error> worldStreamingMountError;
        std::string worldStreamingIndexPath;
        bool worldStreamingPreparing{};
        std::optional<Error> worldSourceError;
        std::optional<Error> worldLayerFailure; /**< Canonical layer failure; presentation resolves localized copy. */

        PartitionedWorldSnapshotLease worldDocument; /**< Immutable capture from the controller's canonical owner. */
        std::optional<WorldBrowserSnapshot> worldBrowser;
        WorldBrowserQuery worldBrowserQuery;
        std::optional<Error> worldDocumentError;
        std::string worldDocumentSource;
        bool worldBakeAvailable{};
        std::optional<OperationId> worldBakeOperation;
        bool worldBakeCurrent{}; /**< Exact published output matches current source and project capture. */
        bool audioPreviewAvailable{};
        std::optional<AudioComponentPreviewSnapshot> audioPreview;
        std::optional<Error> audioPreviewFailure;
        std::vector<NavigationDefinitionEditorTab> navigationDocuments;
        std::string projectRoot;
        std::string projectName;                   /**< Display name projected from canonical project metadata. */
        std::filesystem::path navigationScenePath; /**< Exact project-relative Scene namespace for saved navigation overlays. */
        Assets::AssetRegistryRevision assetRegistryRevision{};
        ContentBrowserDirectory contentBrowser;
        std::string contentBrowserOperationError;
        ContentBrowserClipboardState contentBrowserClipboard;
        bool contentBrowserCanNavigateBack{false};
        bool contentBrowserCanNavigateForward{false};
        DocumentRevision documentRevision;
        AssetSceneDropFence assetDropFence;
        std::uint64_t inspectorDocumentGeneration{};
        std::uint64_t inspectorBehaviorRegistryRevision{};
        std::uint64_t inspectorApprovalRevision{};
        std::uint64_t selectionRevision{};
        std::vector<InspectorProviderApproval> inspectorApprovals;
        SceneTransactionContext physicsContext;
        std::vector<PhysicsProfileChoice> physicsProfiles;
        std::vector<PhysicsMaterialChoice> physicsMaterials;
        std::vector<NavigationDefinitionSnapshot> navigationDefinitions;
        std::string navigationEditError;
        std::uint64_t physicsSelectionRevision{};
        std::string physicsEditError;
        std::vector<ColliderPresentation> viewportColliders;
        std::vector<ColliderEditTarget> selectedColliders;
        std::uint64_t colliderSelectionRevision{};
        std::vector<SceneObject> objects;
        std::shared_ptr<const PrefabHierarchyProjection> prefabHierarchy; /**< Owner-published bounded cache; no Draw I/O. */
        std::optional<PrefabHierarchyTarget> prefabSelection;
        std::optional<Error> prefabHierarchyError;
        std::shared_ptr<const PrefabCreationOutcome> prefabCreationOutcome;
        std::optional<Error> prefabPostCommitError;
        bool prefabHierarchyUnavailable{}; /**< Preparation failed; retained rows are inspection-only, never a fresh publication. */
        std::vector<Gameplay::BehaviorDescriptor> availableBehaviors;
        std::uint64_t availableBehaviorsRevision{}; /**< Owner publication fence for menu/schema projections. */
        std::optional<SceneObjectId> hierarchyRevealObject;
        DocumentRevision hierarchyRevealRevision{};
        std::optional<SceneObjectId> primarySelection;
        std::vector<SceneObjectId> selectedObjects;
        EditorTransformTool activeTransformTool{EditorTransformTool::Select};
        EditorTransformSpace activeTransformSpace{EditorTransformSpace::Local};
        EditorViewportCamera viewportCamera;
        std::optional<Math::Mat4> primarySelectionWorldTransform;
        std::optional<Math::Mat4> primarySelectionPreviewWorldTransform;
        std::optional<Math::Mat4> primarySelectionParentWorldTransform;
        std::optional<Math::Aabb> primarySelectionWorldBounds;
        std::vector<ViewportLightPresentation> viewportLights;
        bool isDirty = false;
        bool sceneExternalConflict = false;
        bool recoveryAvailable = false;
        bool canUndo = false;
        bool canRedo = false;
        EditorPlayState playState{EditorPlayState::Idle};
        EditorPlayViewSource playViewSource{EditorPlayViewSource::AuthoredCamera}; /**< Actual host-selected private projection. */
        std::string playError;
        float fps = 0.0F;

        std::string activeLeftPanelId = "horo.hierarchy";
        std::string activeRightPanelId = "horo.inspector";
        std::string activeLeftTopPanelId;
        std::string activeLeftBottomPanelId;
        std::string activeRightTopPanelId;
        std::string activeRightBottomPanelId;
        SideDockMode leftDockMode = SideDockMode::Full;
        SideDockMode rightDockMode = SideDockMode::Full;
        std::string activeBottomPanelId = "horo.global_dock";
        std::string activeBottomLeftPanelId;
        std::string activeBottomRightPanelId;
        BottomDockMode bottomDockMode = BottomDockMode::Full;
        std::string activeDocumentPanelId = "horo.viewport";
        std::vector<SequenceWorkspaceDocument> sequenceDocuments;
        std::vector<PCGWorkspaceDocument> pcgDocuments;
        std::vector<PCGPaletteEntry> pcgPalette;
        PCG::PCGNodeCatalogSnapshot pcgSchemas;
        std::vector<SourceDocumentSnapshot> sourceDocuments;
        std::vector<UiCanvasWorkspaceDocument> uiCanvasDocuments;
        std::vector<std::shared_ptr<const TerrainDocumentHost>> terrainDocuments;
        std::optional<Error> terrainDocumentError;
        std::vector<PrefabDocumentSnapshot> prefabDocuments;
        std::optional<Error> prefabDocumentError;
        bool prefabRecoveryAvailable{};
        std::shared_ptr<const DocumentRecoveryRecord> prefabRecovery;
        std::vector<SourceHistoryState> sourceHistory;
        std::optional<Result<SourceEditingPublication>> sourceEditingResult;

        float leftPanelWidth = 268.0F;
        float rightPanelWidth = 300.0F;
        float bottomPanelHeight = 250.0F;

        std::unordered_map<PanelId, WorkspaceDockArea, TransparentStringHash, std::equal_to<>> panelDockAreas;

        ActivityBarLayout activityBarLayout;
        WorkspacePanelHost workspacePanelHost;
    };

}  // namespace Horo::Editor
