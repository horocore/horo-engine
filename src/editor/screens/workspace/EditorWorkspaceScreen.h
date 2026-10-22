#pragma once

/** @file EditorWorkspaceScreen.h
 * @brief Target-private workspace host composition, frame routing and leave lifecycle.
 */
#include "EditorWorkspaceView.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorServiceRegistry.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSnackbarHost.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/input/EditorInputActions.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor::WorkspaceScreenDetail {
    struct WorkspacePanelServices {
        IEditorGuiRenderer *guiRenderer{nullptr};
        IEditorViewportRenderer *viewportRenderer{nullptr};
        const Log::IStructuredLogQuery *logQuery{nullptr};
        const IBuildOutputQuery *buildOutputQuery{nullptr};
        const Application::GameplayBuildService *gameplayBuilds{nullptr};
        const IOperationQuery *operationQuery{nullptr};
        IOperationControl *operationControl{nullptr};
        const Application::INetworkDebuggerQuery *networkDebuggerQuery{};
        Application::INetworkDebuggerControl *networkDebuggerControl{};
    };

    struct WorkspaceProjectServices {
        Assets::AssetRegistry *assetRegistry{nullptr};
        CollisionInventoryEvidence *collisionInventory{};
        CollisionActivationEvidence *collisionActivation{};
        const EditorPreviewCollisionReceiptSource *previewCollisionReceipt{};
        const Assets::AssetImporterCatalogSnapshot *importerCatalog{nullptr};
        ProjectMutationCoordinator *mutations{nullptr};
        DurableFileSystem *durableFiles{nullptr};
    };

    struct WorkspacePublishedRevisions {
        DocumentRevision scene{};
        SelectionRevision selection{};
        ViewportRevision viewport{};
        std::uint64_t viewportScene{0};
    };

    /** @brief Workspace-scoped input ownership; frames are released before their context, and the router is borrowed. */
    struct WorkspaceInputState {
        explicit WorkspaceInputState(Input::InputRouter &inputRouter)
            : router(inputRouter),
              context(router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace)) {}

        Input::InputRouter &router;
        Input::InputContextToken context;
        Input::GameplayInputFrameBuilder gameplayFrames{Input::ActionId{kGameplayMoveAction}, {}, {}, {}};
        std::optional<Input::InputBindingProfile> previousProfile;
    };

    /** @brief Borrowed contribution catalogs; application-owned panel/status registrations outlive the screen. */
    struct WorkspaceContributionRegistries final {
        WorkspacePanelRegistry &panels;
        EditorStatusItemRegistry &statusItems;
        bool workspacePersistenceAdmitted{};
    };

    /** @brief Exact pending leave decision captures; borrowed collision authority never extends project lifetime. */
    struct WorkspaceLeaveCapture final {
        /** @brief Requires every prefab owner, source revision and accepted base captured by leave consent. */
        Result<void> ValidateLeavePrefabs(EditorWorkspaceController &controller) const;
        /** @brief Rechecks exact world consent after operations on other owners that may run host callbacks. */
        Result<void> ValidateLeaveWorld(EditorWorkspaceController &controller) const;
        /** @brief Captures the exact cached collision draft for a leave decision. */
        Result<void> CaptureLeaveCollision(EditorWorkspaceController &controller);
        /** @brief Rejects a delayed decision after the collision draft changes. */
        Result<void> ValidateLeaveCollision(EditorWorkspaceController &controller) const;
        /** @brief Rejects a delayed decision after any graph session or revision changes. */
        Result<void> ValidateLeavePCGDocuments(EditorWorkspaceController &controller) const;

        std::vector<SourceDocumentSnapshot> sourceDocuments;
        std::vector<PCGDocumentSnapshot> pcgDocuments;
        std::vector<PrefabDocumentSnapshot> prefabDocuments;
        std::weak_ptr<CollisionProjectSettingsSession> collisionOwner;
        std::shared_ptr<const Application::ProjectCollisionSettings> collisionRoot;
        std::optional<WorldDocumentFence> worldFence;
    };

    /** @brief Routes source document commands while fencing delayed close decisions to the workspace session. */
    class WorkspaceSourceDocumentCommands final {
    public:
        /** @brief Starts the close-decision lifetime after controller preparation. */
        void BeginSession();
        /** @brief Expires delayed close decisions before the controller is retired. */
        void EndSession();
        /** @brief Routes active source Save/Undo/Redo before scene fallback; the controller may be absent. */
        bool HandleMenuAction(EditorWorkspaceController *controller, const EditorMenuAction action) const;
        /** @brief Borrows the current owner and modal services to capture an exact dirty source revision. */
        bool OpenCloseModal(EditorWorkspaceController &controller, const EditorWorkspaceViewCommandData &command,
                            EditorModalHost &modalHost, const EditorGuiContext &context);
        /** @brief Routes current-frame input through the borrowed workspace owner and input context. */
        void RouteInput(Input::InputRouter &router, const Input::InputContextToken &inputContext,
                        const EditorWorkspaceController &controller, EditorWorkspaceViewCommandData &command) const;

    private:
        std::shared_ptr<const void> closeLifetime_;
    };

    /** @brief Owns workspace presentation and controller lifetime inside the GUI host. */
    class EditorWorkspaceScreen final : public GuiScreen {
    public:
        /** @brief Binds borrowed application capabilities before any project activation. */
        explicit EditorWorkspaceScreen(const EditorServiceRegistry &services, PackageWorkspaceAccess packages);
        /** @brief Reports the workspace route identity. */
        ScreenId Id() const override;
        /** @brief Activates a reserved project session after controller preparation. */
        Result<void> OnEnter(const GuiRoute &route) override;
        /** @brief Returns navigation settings from the current controller. */
        Result<NavigationProjectSettingsSession *> NavigationSettings() override;
        /** @brief Updates canonical owners and derived presentation for the current frame. */
        void OnUpdate(float dt) override;
        /** @brief Captures gameplay input only while Play owns simulation. */
        void OnInputSnapshot() override;
        /** @brief Delivers the captured gameplay command to the fixed simulation tick. */
        void OnFixedUpdate(const std::uint64_t simulationTick, const double fixedDeltaSeconds) override;
        /** @brief Draws the workspace before routing its current-frame command and input. */
        void Draw(const GuiContentRegion &contentRegion) override;
        /** @brief Collects only currently presented panel identities. */
        void CollectActivePanelIds(std::vector<std::string_view> &output) const override;
        /** @brief Routes application menu commands through the owning document controller. */
        bool HandleMenuInvocation(const EditorMenuInvocation &invocation) override;
        /** @brief Returns the current collision document session. */
        Result<std::shared_ptr<CollisionProjectSettingsSession>> CollisionSettings() override;
        /** @brief Captures exact dirty owners before requesting a leave resolution. */
        LeaveDecision CanLeave(const LeaveTarget &) const override;
        /** @brief Saves canonical owners before retiring their sessions. */
        Result<LeaveDecision> SaveForLeave();
        /** @brief Admits explicit discard through each document owner shutdown. */
        Result<LeaveDecision> DiscardForLeave();
        /** @brief Applies a leave resolution only while the captured owners remain current. */
        Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) override;
        /** @brief Retires project owners and restores borrowed host input state. */
        void OnLeave() override;

    private:
        /** @brief Queries every canonical authored owner before allowing the workspace leave workflow. */
        bool HasDirtyDocuments() const;
        /** @brief Validates captured leave consent against each independently owned document group. */
        Result<void> ValidateLeaveCapture() const;
        /** @brief Attaches built-in panels before session publication. */
        void AttachWorkspacePanels();
        /** @brief Owns the project path and application closure across asynchronous Play preparation. */
        EditorPlaySceneFactory BindPlayFactory(const std::filesystem::path &projectRoot) const;
        /** @brief Capture gameplay borrows and the selected play factory at the host composition boundary. */
        EditorWorkspaceGameplayServices BindGameplayServices(const std::filesystem::path &projectRoot) const;
        /** @brief Binds the explicit authoring stage capability to this authorized project. */
        EditorPlaySceneFactory BindPrefabStageFactory(const std::filesystem::path &projectRoot) const;
        /** @brief Binds borrowed host services and transfers admitted source/PCG owners into controller dependencies. */
        EditorWorkspaceDependencies BindControllerDependencies(const std::filesystem::path &projectRoot,
                                                               UiInspectorResources *uiInspectorResources,
                                                               const Assets::AssetId &sessionOwner,
                                                               std::shared_ptr<const Assets::IAssetRegistrySource> registrySource,
                                                               PCGEditorCatalog &&pcgCatalog, PCGPreviewInputs &&pcgPreviewInputs) const;
        /** @brief Creates the controller under the uncommitted project activation lease. */
        Result<void> InitializeController(const std::filesystem::path &projectRoot);
        /** @brief Keeps native dialog input ownership through scene publication. */
        bool SaveSceneAs(const bool copyOnly);
        /** @brief Restores admitted extension surface state before presentation. */
        void RestoreExtensionWorkspace();
        /** @brief Persists only an admitted extension workspace through durable file publication. */
        void SaveExtensionWorkspace();
        /** @brief Opens package preparation with the exact project capability. */
        void OpenPackagesModal();
        /** @brief Opens topology editing only while Play is idle. */
        void OpenPlayTopologyProfilesModal();
        /** @brief Captures the external scene before opening its comparison modal. */
        void OpenSceneComparison();
        /** @brief Opens the transient captured-subtree workflow under the workspace lifetime fence. */
        void OpenPrefabCreationModal(const SceneObjectId root);
        /** @brief Resolves a contained destination before opening behavior creation. */
        void OpenGameplayBehaviorModal(const GameplayBehaviorKind kind, const std::string &requestedDirectory);
        /** @brief Installs the project profile while retaining the host profile for teardown. */
        void LoadProjectInputProfile(const std::filesystem::path &projectRoot);
        /** @brief Publishes only changed scene and selection revisions. */
        void PublishViewportSceneIfChanged();
        /** @brief Projects document and selection status through registered status items. */
        void UpdateStatusItems();

        PackageWorkspaceAccess packages_;
        GuiScreenHost &host_;
        const EditorServiceRegistry &services_;
        EditorModalHost &modalHost_;
        WorkspaceSourceDocumentCommands sourceCommands_;
        std::shared_ptr<const void> workspaceControllerLifetime_;
        const EditorGuiContext &context_;
        WorkspaceContributionRegistries contributions_;
        WorkspaceInputState input_;
        EditorWorkspaceView view_;
        EditorViewportSceneState &viewportSceneState_;
        Runtime::RuntimeSceneService &runtimeScene_;
        EditorSettingsService &settings_;
        ProjectOpenService &projectOpenService_;
        WorkspacePanelServices panelServices_{};
        WorkspaceProjectServices projectServices_{};
        WorkspacePublishedRevisions publishedRevisions_{};
        std::unique_ptr<UiInspectorResources> uiInspectorResources_;
        std::unique_ptr<EditorWorkspaceController> controller_;
        mutable WorkspaceLeaveCapture leaveCapture_;
        std::unique_ptr<EditorSnackbarHost> snackbarHost_;
    };
}  // namespace Horo::Editor::WorkspaceScreenDetail
