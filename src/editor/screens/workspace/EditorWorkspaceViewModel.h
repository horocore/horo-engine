#pragma once

#include "EditorPlaySceneHost.h"
#include "EditorPlayState.h"
#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/DecisionControl.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/AI/EnvironmentQueryAuthoring.h"
#include "Horo/Editor/ActivityBarLayout.h"
#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/BlackboardSchemaDocument.h"
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
#include "Horo/Runtime/Input.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "editor/document/SceneDocument.h"
#include "editor/project_model/EditorTransformTool.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "editor/screens/workspace/ContentBrowserModel.h"
#include "editor/screens/workspace/DecisionGraphLayout.h"
#include "editor/screens/workspace/DecisionStateMachineAuthoring.h"
#include "editor/screens/workspace/EnvironmentQueryDocumentFence.h"
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
#include <utility>
#include <variant>
#include <vector>

namespace Horo::AI {
    class DecisionAssetValidationReport;
    class DecisionBlackboardProviderMetadata;
    class EnvironmentQueryAsset;
    struct QueryDebugSnapshot;
    struct DecisionGraphSource;
    class QuerySchemaRegistry;
}  // namespace Horo::AI

namespace Horo::Application {
    struct TerrainAuthoringCookCompletion;
    struct TerrainAuthoringAssetCreated;
    struct BlackboardUsageReview;
}  // namespace Horo::Application

namespace Horo::Editor {
    class SourceReviewOverlay;
    struct SourceReviewApplication;
    class TerrainDocumentHost;
    struct PrefabApplyRecoveryCandidate;

    /** @brief Workspace presentation of the paired authoring recovery route. */
    enum class PrefabApplyRecoveryNotice : std::uint8_t {
        None,
        Reopen,
        Invalid,
        Blocked,
        Failed,
    };
    struct TerrainToolCommand;
    struct TerrainToolSelection;
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
        DiscardAndClose,
        CompareExternal,
        KeepExternalDraft,
        SaveAs
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
        ReviewSourceProposal,
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
        UiCanvasRecover,
        UiCanvasDiscardRecovery,
        SaveScene,
        SaveSceneAs,
        SaveSceneCopyAs,
        CompareExternalScene,
        ReloadExternalScene,
        OverwriteExternalScene,
        RestoreSceneRecovery,
        ReviewPrefabApplyRecovery,
        RestorePrefabApplyRecovery,
        DiscardSceneRecovery,
        UndoScene,
        RedoScene,
        StartPlay,
        NavigatePlayDecisionProblem,
        ControlPlayDecision,
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
        CreateEnvironmentQuery,
        CreateBlackboardSchema,
        CreateStateMachine,
        CreateTaskDecision,
        CreateLuaBehavior,
        CreateNativeBehavior,
        ReimportContentBrowserAsset,
        RevealContentBrowserEntry,
        OpenSourceFile,
        OpenRecentAiAsset,
        PreviewEnvironmentQuery,
        EditEnvironmentQuery,
        EditDecisionStateMachine,
        EditDecisionBehaviorTree,
        EditDecisionUtilityGraph,
        EditDecisionLayout,
        ResolveDecisionExternalChange,
        OpenDecisionDiagnosticKey,
        ReloadExternalDecisionGraph,
        MergeExternalDecisionGraph,
        KeepLocalDecisionGraph,
        RestoreDecisionGraphRecovery,
        DiscardDecisionGraphRecovery,
        OpenDiagnosticSource,
        ChangeActivePanel,
        CloseWorkspacePanel,
        ReorderActivityBarItem,
        DockWorkspacePanel,
        ResizePanel,
        OpenPlayTopologyProfiles,
        TerrainTool,
        CreateTerrainLayerSet,
        CreateTerrainFoliageType,
        OpenGyroscopeBinding,
        BlackboardDocument,
        ApplySourceReview,
        UndoSourceReview,
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

    /** @brief Confirmed creation with an explicit directory and typed native query options. */
    struct EnvironmentQueryCreationRequest final {
        std::string directory;
        std::string name;
        AI::QueryPointLattice lattice;
        std::uint32_t maximumResults{};
        std::optional<AI::QueryPointDistanceTest> distance;
        std::optional<AI::QueryPerceptionMatch> perception; /**< Present for an Actor Perception query. */
    };

    /** @brief Result of checking a saved query against the application's current descriptor registry. */
    enum class QueryDescriptorAvailability : std::uint8_t {
        RegistryUnavailable,
        Incompatible,
        Validated
    };

    /** @brief Owner-published state of a read-only Scene preview. */
    enum class EnvironmentQueryPreviewStatus : std::uint8_t {
        Idle,
        Running,
        Ready,
        Stale,
        Cancelled,
        Failed
    };

    /** @brief Exact authored Scene and installed preview scene pinned before worker submission. */
    struct EnvironmentQueryPreviewScene final {
        DocumentRevision document;
        SceneObjectId anchor;
        Runtime::SceneRuntimeId runtime;
        std::uint64_t structure{};
        std::optional<SceneObjectId> target; /**< The distinct secondary Scene selection when required by the query. */
    };

    /** @brief Workspace-only evidence, with no runtime apply or navigation capability. */
    struct EnvironmentQueryPreviewView final {
        EnvironmentQueryPreviewStatus status{EnvironmentQueryPreviewStatus::Idle};
        EnvironmentQueryPreviewScene scene;
        std::shared_ptr<const AI::QueryDebugSnapshot> snapshot;
        std::optional<Error> diagnostic;
    };

    /** @brief One workspace-only preview action. */
    enum class EnvironmentQueryPreviewAction : std::uint8_t {
        Run,
        Cancel,
        FocusCandidate
    };

    /** @brief A request correlated to the exact saved query and current authored Scene. */
    struct EnvironmentQueryPreviewRequest final {
        DocumentInstanceId instance;
        Sha256Digest source;
        DocumentRevision scene;
        std::optional<SceneObjectId> anchor;
        EnvironmentQueryPreviewAction action{EnvironmentQueryPreviewAction::Run};
        std::optional<SceneObjectId> target;
        std::shared_ptr<const AI::QueryDebugSnapshot> previewEvidence; /**< Exact presented publication for candidate navigation. */
        std::optional<std::uint32_t> candidate;
    };

    /** @brief Ordinary semantic edit and history actions from the saved Query tab. */
    enum class EnvironmentQueryEditAction : std::uint8_t {
        Apply,
        Undo,
        Redo,
        Save,
        ReloadExternal,
        MergeExternal,
        KeepLocal,
        RestoreRecovery,
        DiscardRecovery
    };

    /** @brief An optional structural edit: append a typed test or remove one exact stage. */
    using EnvironmentQueryDistanceStageChange = std::variant<std::monostate, AI::QueryPointDistanceTest, AI::QueryStageId>;

    /** @brief A typed transaction correlated to exactly one authoring revision. */
    struct EnvironmentQueryEditRequest final {
        EnvironmentQueryDocumentFence fence;
        EnvironmentQueryEditAction action{EnvironmentQueryEditAction::Apply};
        std::optional<AI::BuiltinPointQuerySettings> settings;
        EnvironmentQueryDistanceStageChange stageChange;
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
        std::optional<AI::BuiltinPerceptionQuerySettings> perception;
    };

    /** @brief An immutable, registered Environment Query source in a normal workspace document. */
    struct EnvironmentQueryWorkspaceDocument final {
        DocumentIdentity identity;
        Assets::AssetId asset;
        Sha256Digest source;
        std::shared_ptr<const AI::EnvironmentQueryAsset> query;
        QueryDescriptorAvailability descriptors{QueryDescriptorAvailability::RegistryUnavailable};
        EnvironmentQueryPreviewView preview;
        std::optional<AI::BuiltinPointQuerySettings> settings;
        std::optional<AI::BuiltinPerceptionQuerySettings> perception;
        std::uint64_t revision{1};
        bool dirty{};
        bool canUndo{};
        bool canRedo{};
        bool conflict{};
        std::optional<Error> diagnostic;
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
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
        std::optional<UiCanvasPreviewBox> selectedLayout;
        std::optional<Error> diagnostic;
        std::optional<Runtime::Ui::UiDiagnosticRecord> problem;
        Runtime::Ui::UiStylePropertyId problemStyleProperty;
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
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
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

    /** @brief Optional exact binding replacement; absence in a request means append without replacing peers. */
    struct GyroscopeBindingReplacement final {
        std::size_t index{};
        Input::InputBinding expected;
    };

    /** @brief Owned user intent for one revision-fenced gyro creation/edit modal. */
    struct GyroscopeBindingEditRequest final {
        Input::ActionId action;
        std::uint64_t configurationRevision{};
        std::optional<GyroscopeBindingReplacement> replacement;
    };

    /** @brief One exact saved blackboard option available to the initial decision source author. */
    struct DecisionSchemaChoice final {
        Assets::AssetId asset;
        std::string path;
    };

    /** @brief Confirmed initial State Machine with an explicit saved schema and terminal outcome. */
    struct StateMachineCreationRequest final {
        std::string directory;
        std::string name;
        Assets::AssetId schema;
        AI::DecisionStateTerminal outcome{AI::DecisionStateTerminal::Succeeded};
    };

    /** @brief Authoring restrictions derived from the current provider catalog. */
    enum class DecisionPaletteAdmission : std::uint8_t {
        Allowed,
        UnsupportedRole,
        CapabilitiesUnavailable,
        PortWiringRequired,
        UnsupportedProperties,
        ImplementationUnavailable
    };

    /** @brief Inert label, exact identity and authoring status of a registered provider. */
    struct DecisionTaskChoice final {
        AI::DecisionNodeTypeId type;
        AI::DecisionProviderId provider;
        std::uint32_t version{};
        std::string label;
        bool queryOutput{};
        AI::AiCapabilitySet capabilities{};
        AI::DecisionDescriptorSourceKind source{AI::DecisionDescriptorSourceKind::Native};
        AI::DecisionNodeRole role{AI::DecisionNodeRole::Task};
        DecisionPaletteAdmission admission{DecisionPaletteAdmission::Allowed};
        std::vector<AI::BehaviorTreeProperty> editableProperties{};
    };

    /** @brief Explicit first Utility score input from a saved scalar key. */
    struct InitialUtilityScore final {
        AI::BlackboardKeyId key;
        Sha256Digest schemaSource; /**< Exact saved schema shown when the key was offered. */
        double minimum{};
        double maximum{1};
    };

    /** @brief Confirmed first Behavior Tree or Utility with an exact native task and registry revision. */
    struct TaskDecisionCreationRequest final {
        std::string directory;
        std::string name;
        Assets::AssetId schema;
        AI::DecisionPlanKind family{AI::DecisionPlanKind::BehaviorTree};
        AI::DecisionNodeTypeId task;
        AI::DecisionProviderId provider;
        std::uint32_t version{};
        std::uint64_t catalogRevision{};
        std::optional<InitialUtilityScore> score;
    };

    /** @brief Saved inputs and compatible writable keys for the reviewed task and revision. */
    struct BehaviorTreeQueryReview final {
        Assets::AssetId asset;
        AI::DecisionNodeId node;
        std::uint64_t documentRevision{};
        Sha256Digest queryDigest;
        Sha256Digest schemaDigest;
        AI::QueryId query;
        AI::QueryResultSchemaId result;
        std::uint32_t resultVersion{};
        AI::QueryItemTypeId item;
        std::vector<std::pair<AI::BlackboardKeyId, std::string>> keys;
        std::optional<DecisionTaskSelection> task;
    };

    /** @brief Explicit choices for the reviewed Decision source pair. */
    enum class DecisionExternalAction : std::uint8_t {
        Review,
        Reload,
        Merge,
        KeepDraft,
        AcceptDiskPair
    };

    namespace DecisionExternalErrors {
        extern const ErrorCodeDescriptor Overlap;
    }

    struct DecisionExternalReview final {
        Assets::AssetSourceIdentity source;
        Assets::AssetSourceIdentity layout;
        std::uint64_t revision{};
        bool graphChanged{};
        bool layoutChanged{};
        std::optional<Assets::AssetSourceIdentity> interruptedSave;
        bool operator==(const DecisionExternalReview &) const = default;
    };

    struct DecisionExternalRequest final {
        DecisionGraphDocumentFence fence;
        DecisionExternalAction action{DecisionExternalAction::Review};
        std::optional<DecisionExternalReview> reviewed;
    };

    /** @brief Immutable authored source projection independently of runtime graphs. */
    struct DecisionGraphWorkspaceDocument final {
        DocumentIdentity identity;
        Assets::AssetId asset;
        Sha256Digest source;
        std::shared_ptr<const AI::DecisionGraphSource> graph;
        std::uint64_t revision{};
        bool editable{};
        AI::DecisionPlanKind family{AI::DecisionPlanKind::StateMachine};
        std::shared_ptr<const DecisionGraphLayout> layout;
        bool layoutWritable{};
        std::optional<BehaviorTreeSubtreeReview> subtreeReview;
        std::optional<BehaviorTreeQueryReview> queryReview;
        bool dirty{};
        bool canUndo{};
        bool canRedo{};
        std::optional<Error> diagnostic;
        std::shared_ptr<const AI::DecisionAssetValidationReport> validation; /**< Typed targets from the last failed validation. */
        bool conflict{};
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
        std::optional<DecisionExternalReview> externalReview;
        std::optional<Assets::AssetSourceIdentity> interruptedSave;
    };

    /** @brief Confirmed first Blackboard asset and an explicitly authored initial key. */
    struct BlackboardCreationRequest final {
        std::string directory;
        std::string name;
        std::string keyName;
        AI::BlackboardKeyDescriptor key;
    };

    /** @brief Commands emitted by Blackboard presentation for an exact document revision. */
    enum class BlackboardDocumentOperation : std::uint8_t {
        Edit,
        Undo,
        Redo,
        Save,
        Reload,
        ReloadDiscardingDraft,
        MergeExternal,
        KeepLocal,
        RestoreRecovery,
        DiscardRecovery,
        ReviewUsages,
        ApplyReviewedEdit
    };

    struct BlackboardWorkspaceRequest final {
        BlackboardDocumentFence fence;
        BlackboardDocumentOperation operation{BlackboardDocumentOperation::Edit};
        std::optional<BlackboardSchemaEdit> edit;
        std::optional<AI::BlackboardKeyId> reviewKey;
        std::optional<AI::BlackboardKeyId> replacement;
        std::shared_ptr<const Application::BlackboardUsageReview> expectedReview;
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
    };

    struct BlackboardWorkspaceDocument final {
        BlackboardSchemaDocumentSnapshot snapshot;
        std::optional<Error> diagnostic;
        bool usageReviewAvailable{};
        bool reviewingUsages{};
        std::shared_ptr<const Application::BlackboardUsageReview> usageReview;
        std::optional<BlackboardReferenceEditPreview> referencePreview;
        bool referenceEditReady{};
        bool referenceEditStaged{};
        AI::BlackboardKeyId diagnosticKey;
        std::uint64_t diagnosticKeyRequest{};
        std::shared_ptr<const DocumentRecoveryRecord> recovery;
    };

    /** @brief Requests the exact Blackboard target from a published Decision validation finding. */
    struct DecisionDiagnosticKeyRequest final {
        DecisionGraphDocumentFence fence;
        AI::BlackboardSchemaId schema;
        AI::BlackboardKeyId key;
    };

    /** @brief Exact typed entity navigation issued for the on-screen Decision problem. */
    struct PlayDecisionNavigationRequest final {
        std::uint64_t tick{};
        AI::AgentId agent;
        Runtime::EntityRef entity;
    };

    struct EditorWorkspaceViewCommandData {  // NOSONAR(cpp:S1820) Command payload variant container
        EditorWorkspaceViewCommand command = EditorWorkspaceViewCommand::None;
        std::optional<GyroscopeBindingEditRequest> gyroscopeBinding;
        std::optional<WorldBrowserQuery> worldBrowserQuery;
        std::optional<WorldDocumentFence> worldDocumentFence;
        std::optional<OperationId> worldBakeOperation; /**< Exact accepted operation targeted by a cancellation intent. */
        std::optional<WorldSourceToolCommand> worldSource;
        std::shared_ptr<const TerrainToolCommand> terrainTool;
        std::optional<WorldLayerOutlinerIntent> worldLayerIntent;
        std::optional<DocumentInstanceId> documentInstance;
        std::optional<PlayDecisionNavigationRequest> playDecisionNavigation;
        std::optional<AI::AgentHandle> visiblePlayDecisionInspector;
        std::optional<EditorPlayDecisionDebugCommand> playDecisionDebugCommand;
        std::optional<DocumentInstanceId> documentMoveBefore;
        std::optional<PrefabDocumentFence> prefabFence;
        std::shared_ptr<const DocumentRecoveryRecord> prefabRecovery;
        std::shared_ptr<const PrefabApplyRecoveryCandidate> prefabApplyRecovery;
        std::optional<PrefabSourceEdit> prefabEdit;
        std::shared_ptr<const PrefabExternalComparison> prefabExternalComparison;
        std::optional<std::filesystem::path> prefabSaveDestination;
        PrefabDocumentAction prefabAction{PrefabDocumentAction::Save};
        std::optional<SourceEditingCommand> sourceInteraction;
        std::shared_ptr<const SourceReviewOverlay> sourceReview;
        bool sourceTextInputOwned{}; /**< Current-frame ownership, independent of whether text changed. */
        std::optional<SourceSaveRequest> sourceSave;
        SourceCloseDecision sourceCloseDecision{SourceCloseDecision::Cancel};
        UiCanvasDocumentStateId uiCanvasState;
        std::shared_ptr<const DocumentRecoveryRecord> uiCanvasRecovery;
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
        std::optional<BlackboardWorkspaceRequest> blackboard;
        std::optional<EnvironmentQueryCreationRequest> newEnvironmentQuery;
        std::optional<BlackboardCreationRequest> newBlackboard;
        std::optional<StateMachineCreationRequest> newStateMachine;
        std::optional<TaskDecisionCreationRequest> newTaskDecision;
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
        std::optional<AudioComponentPreviewOutputSwitch> audioPreviewOutputSwitch;
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
        std::optional<Assets::AssetId> recentAiAsset = std::nullopt;
        std::optional<EnvironmentQueryPreviewRequest> environmentQueryPreview;
        std::optional<EnvironmentQueryEditRequest> environmentQueryEdit;
        std::optional<StateMachineEditRequest> stateMachineEdit = std::nullopt;
        std::optional<BehaviorTreeEditRequest> behaviorTreeEdit = std::nullopt;
        std::optional<UtilityGraphEditRequest> utilityGraphEdit = std::nullopt;
        std::optional<DecisionGraphLayoutRequest> decisionLayoutEdit = std::nullopt;
        std::optional<DecisionExternalRequest> decisionExternal;
        std::optional<DecisionDiagnosticKeyRequest> decisionDiagnosticKey;
        std::optional<DecisionGraphDocumentFence> decisionGraphFence;
        std::shared_ptr<const DocumentRecoveryRecord> decisionGraphRecovery;
        std::optional<DiagnosticSourceRequest> diagnosticSource = std::nullopt;
        std::optional<CreateGameplayBehaviorRequest> gameplayBehaviorRequest = std::nullopt;
        std::optional<AssetSceneDropRequest> assetSceneDrop = std::nullopt;
        std::shared_ptr<const SourceReviewApplication> sourceReviewApplication;
        SourceEditBatch sourceReviewUndo;
        CancellationToken sourceReviewLifetime;
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

    /** @brief Makes an exact Query edit or history command for the active workspace document. */
    inline EditorWorkspaceViewCommandData MakeEnvironmentQueryEditCommand(const EnvironmentQueryWorkspaceDocument &document,
                                                                          EnvironmentQueryEditAction action,
                                                                          std::optional<AI::BuiltinPointQuerySettings> settings = {},
                                                                          EnvironmentQueryDistanceStageChange stageChange = {}) {
        return {.command = EditorWorkspaceViewCommand::EditEnvironmentQuery,
                .environmentQueryEdit = EnvironmentQueryEditRequest{{document.identity.instance, document.revision},
                                                                    action,
                                                                    std::move(settings),
                                                                    stageChange,
                                                                    document.recovery}};
    }

    /** @brief Routes the shared Decision save/undo/redo action to the focused family. */
    inline EditorWorkspaceViewCommandData MakeDecisionHistoryCommand(const DecisionGraphWorkspaceDocument &document,
                                                                     DecisionHistoryAction action) {
        if (document.family == AI::DecisionPlanKind::Utility) {
            auto operation = UtilityGraphEditOperation::Save;
            if (action == DecisionHistoryAction::Undo)
                operation = UtilityGraphEditOperation::Undo;
            if (action == DecisionHistoryAction::Redo)
                operation = UtilityGraphEditOperation::Redo;
            return {.command = EditorWorkspaceViewCommand::EditDecisionUtilityGraph,
                    .utilityGraphEdit = UtilityGraphEditRequest{.instance = document.identity.instance,
                                                                .revision = document.revision,
                                                                .operation = operation}};
        }
        if (document.family == AI::DecisionPlanKind::BehaviorTree) {
            auto operation = BehaviorTreeEditOperation::Save;
            if (action == DecisionHistoryAction::Undo)
                operation = BehaviorTreeEditOperation::Undo;
            if (action == DecisionHistoryAction::Redo)
                operation = BehaviorTreeEditOperation::Redo;
            return {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                    .behaviorTreeEdit = BehaviorTreeEditRequest{.instance = document.identity.instance,
                                                                .revision = document.revision,
                                                                .operation = operation}};
        }
        auto operation = StateMachineEditOperation::Save;
        if (action == DecisionHistoryAction::Undo)
            operation = StateMachineEditOperation::Undo;
        if (action == DecisionHistoryAction::Redo)
            operation = StateMachineEditOperation::Redo;
        return {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                .stateMachineEdit =
                    StateMachineEditRequest{.instance = document.identity.instance, .revision = document.revision, .operation = operation}};
    }

    /** @brief Project-workspace recency keyed by a registered asset identity and type. */
    struct RecentAiAsset final {
        Assets::AssetId asset;
        Assets::AssetTypeId type;
        std::string path;
        bool available{true};
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
        std::vector<RecentAiAsset> recentAiAssets;
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
        std::shared_ptr<const PrefabApplyRecoveryCandidate> prefabApplyRecovery;
        PrefabApplyRecoveryNotice prefabApplyRecoveryNotice{};
        bool canUndo = false;
        bool canRedo = false;
        EditorPlayState playState{EditorPlayState::Idle};
        EditorPlayViewSource playViewSource{EditorPlayViewSource::AuthoredCamera}; /**< Actual host-selected private projection. */
        bool simulatorPreviewAvailable{}; /**< Actual current consumed simulator view; false forbids stale texture display. */
        std::string playError;
        std::optional<EditorPlayDecisionProblem> playDecisionProblem;
        std::vector<EditorPlayAgentInspection> playAgentInspection;
        std::optional<EditorPlayDecisionDebugState> playDecisionDebug;
        bool playDecisionObjectAvailable{};
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
        std::vector<EnvironmentQueryWorkspaceDocument> environmentQueryDocuments;
        bool environmentQueryCreationAvailable{};
        std::vector<BlackboardWorkspaceDocument> blackboardDocuments;
        bool blackboardCreationAvailable{};
        std::vector<DecisionSchemaChoice> decisionSchemaChoices;
        std::vector<DecisionSchemaChoice> behaviorTreeSubtreeChoices;
        std::vector<DecisionSchemaChoice> behaviorTreeQueryChoices;
        std::vector<DecisionTaskChoice> decisionTaskChoices;
        std::vector<DecisionTaskChoice> decisionTaskPaletteChoices; /**< All observed contracts, including unavailable authoring roles. */
        std::vector<DecisionTaskChoice> behaviorTreeTaskChoices;
        std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> decisionProviderMetadata; /**< Detached current Project catalog. */
        bool decisionPackageActivationTrustRequired{}; /**< Current package activation failure from the exact Project package owner. */
        std::vector<DecisionGraphWorkspaceDocument> decisionGraphDocuments;
        std::vector<PCGWorkspaceDocument> pcgDocuments;
        std::vector<PCGPaletteEntry> pcgPalette;
        PCG::PCGNodeCatalogSnapshot pcgSchemas;
        std::vector<SourceDocumentSnapshot> sourceDocuments;
        std::vector<UiCanvasWorkspaceDocument> uiCanvasDocuments;
        std::vector<std::shared_ptr<const TerrainDocumentHost>> terrainDocuments;
        std::shared_ptr<const TerrainDocumentHost> terrainPaletteDocument;
        std::shared_ptr<const TerrainToolSelection> terrainToolSelection;
        bool terrainCookAvailable{};
        bool terrainCookRunning{};
        bool terrainCookCurrent{};
        bool terrainAssetCreationAvailable{};
        std::uint64_t terrainCookEpoch{};
        DocumentInstanceId terrainCookDocument;
        std::shared_ptr<const Application::TerrainAuthoringCookCompletion> terrainCookOutcome;
        std::shared_ptr<const Application::TerrainAuthoringAssetCreated> terrainAssetCreationOutcome;
        std::optional<Error> terrainDocumentError;
        std::vector<PrefabDocumentSnapshot> prefabDocuments;
        std::optional<Error> prefabDocumentError;
        std::optional<Error> prefabOverrideError;
        bool prefabApplyAvailable{};
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
