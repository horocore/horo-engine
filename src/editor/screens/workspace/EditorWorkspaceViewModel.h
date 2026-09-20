#pragma once

#include "Horo/Editor/ActivityBarLayout.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "editor/document/SceneDocument.h"
#include "editor/project_model/EditorTransformTool.h"
#include "editor/project_model/EditorViewportModel.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "editor/screens/workspace/ContentBrowserModel.h"
#include "editor/screens/workspace/GameplayBehaviorRequest.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Horo::Editor {
    /** @brief Presentation projection of the editor play-session lifecycle. */
    enum class EditorPlayState : std::uint8_t {
        Idle,
        Starting,
        Playing,
        Paused,
        Stopping,
        Failed,
    };

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

    enum class EditorWorkspaceViewCommand {
        None,
        ReturnToWelcome,
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
        InstantiateAsset,
        DuplicateObject,
        DeleteObject,
        DeleteSelectedObjects,
        SelectObject,
        PickViewport,
        NavigateViewport,
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
        UpdateAudioSourceComponent,
        UpdateObjectEditorState,
        AddComponentToObject,
        RemoveComponentFromObject,
        AttachBehaviorToObject,
        UpdateBehaviorOnObject,
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
        ReorderActivityBarItem,
        DockWorkspacePanel,
        ResizePanel,
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

    /** @brief Absolute, optionally line-addressed diagnostic source requested by an output panel. */
    struct DiagnosticSourceRequest {
        std::string absolutePath;
        std::uint32_t line{};
        std::uint32_t column{};
    };

    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    struct EditorWorkspaceViewCommandData {  // NOSONAR(cpp:S1820) Command payload variant container
        EditorWorkspaceViewCommand command = EditorWorkspaceViewCommand::None;
        std::optional<EditorMenuInvocation> menuInvocation = std::nullopt;
        std::optional<ObjectSelectionRequest> objectSelection = std::nullopt;
        std::optional<int> targetIndex = std::nullopt;
        std::optional<SceneObjectId> objectPayload = std::nullopt;
        std::optional<Runtime::PrimitiveId> primitivePayload = std::nullopt;
        std::optional<Math::Transform> transformPayload = std::nullopt;
        std::optional<std::vector<SceneObjectTransformUpdate>> transformUpdates = std::nullopt;
        std::optional<ViewportPickRequest> viewportPickPayload = std::nullopt;
        std::optional<EditorViewportNavigationDelta> viewportNavigationPayload = std::nullopt;
        std::optional<Runtime::CameraProjection> viewportProjectionPayload = std::nullopt;
        std::optional<Runtime::CameraComponent> cameraPayload = std::nullopt;
        std::optional<Runtime::LightComponent> lightPayload = std::nullopt;
        std::optional<Runtime::TriggerVolumeComponent> triggerVolumePayload = std::nullopt;
        std::optional<Runtime::AudioSourceComponent> audioSourcePayload = std::nullopt;
        std::optional<SceneObjectEditorState> editorStatePayload = std::nullopt;
        std::optional<ComponentType> componentTypePayload = std::nullopt;
        std::optional<Gameplay::BehaviorTypeId> behaviorTypePayload = std::nullopt;
        std::optional<Gameplay::BehaviorComponent> behaviorPayload = std::nullopt;
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

    struct EditorWorkspaceViewModel {  // NOSONAR(cpp:S1820) Comprehensive workspace presentation model
        std::string projectRoot;
        Assets::AssetRegistryRevision assetRegistryRevision{};
        ContentBrowserDirectory contentBrowser;
        std::string contentBrowserOperationError;
        ContentBrowserClipboardState contentBrowserClipboard;
        bool contentBrowserCanNavigateBack{false};
        bool contentBrowserCanNavigateForward{false};
        DocumentRevision documentRevision;
        std::vector<SceneObject> objects;
        std::vector<Gameplay::BehaviorDescriptor> availableBehaviors;
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

        float leftPanelWidth = 268.0F;
        float rightPanelWidth = 300.0F;
        float bottomPanelHeight = 310.0F;

        std::unordered_map<PanelId, WorkspaceDockArea, TransparentStringHash, std::equal_to<>> panelDockAreas;

        ActivityBarLayout activityBarLayout;
        WorkspacePanelHost workspacePanelHost;
    };

}  // namespace Horo::Editor
