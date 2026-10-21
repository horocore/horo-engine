#include "EditorWorkspaceScreen.h"

#include "EditorWorkspaceInputRouting.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/NavigationErrors.h"

#include <imgui.h>

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @copydoc EditorWorkspaceScreen::EditorWorkspaceScreen */
    EditorWorkspaceScreen::EditorWorkspaceScreen(const EditorServiceRegistry &services, PackageWorkspaceAccess packages)
        : packages_(std::move(packages)), host_(services.Get<GuiScreenHost>()), services_(services),
          modalHost_(services.Get<EditorModalHost>()), context_(services.GetConst<EditorGuiContext>()),
          contributions_{services.Get<WorkspacePanelRegistry>(), services.Get<EditorStatusItemRegistry>()},
          input_(services.Get<Input::InputRouter>()),
          view_(context_, contributions_.panels, services.Get<std::uintptr_t>(), input_.router, input_.context,
                services.TryGet<Extensions::EditorActivityHost>(), services.TryGet<IEditorGuiRenderer>()),
          viewportSceneState_(services.Get<EditorViewportSceneState>()), runtimeScene_(services.Get<Runtime::RuntimeSceneService>()),
          settings_(services.Get<EditorSettingsService>()), projectOpenService_(services.Get<ProjectOpenService>()),
          panelServices_{
              .guiRenderer = services.TryGet<IEditorGuiRenderer>(),
              .viewportRenderer = services.TryGet<IEditorViewportRenderer>(),
              .logQuery = services.TryGetConst<Log::IStructuredLogQuery>(),
              .buildOutputQuery = services.TryGetConst<IBuildOutputQuery>(),
              .gameplayBuilds = services.TryGetConst<Application::GameplayBuildService>(),
              .operationQuery = services.TryGetConst<IOperationQuery>(),
              .operationControl = services.TryGet<IOperationControl>(),
              .networkDebuggerQuery = services.TryGetConst<Application::INetworkDebuggerQuery>(),
              .networkDebuggerControl = services.TryGet<Application::INetworkDebuggerControl>(),
          },
          projectServices_{
              .assetRegistry = services.TryGet<Assets::AssetRegistry>(),
              .collisionInventory = services.TryGet<CollisionInventoryEvidence>(),
              .collisionActivation = services.TryGet<CollisionActivationEvidence>(),
              .previewCollisionReceipt = services.TryGetConst<EditorPreviewCollisionReceiptSource>(),
              .importerCatalog = services.TryGetConst<Assets::AssetImporterCatalogSnapshot>(),
              .mutations = services.TryGet<ProjectMutationCoordinator>(),
              .durableFiles = services.TryGet<DurableFileSystem>(),
          } {}

    /** @copydoc EditorWorkspaceScreen::Id */
    ScreenId EditorWorkspaceScreen::Id() const {
        return static_cast<ScreenId>(GuiRouteKind::EditorWorkspace);
    }

    /** @copydoc EditorWorkspaceScreen::OnEnter */
    Result<void> EditorWorkspaceScreen::OnEnter(const GuiRoute &route) {
        if (!std::holds_alternative<EditorWorkspaceRouteParameters>(route.parameters)) {
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidRouteParameters));
        }
        const auto &params = std::get<EditorWorkspaceRouteParameters>(route.parameters);
        auto reserved = projectOpenService_.ReserveSession(params.session);
        if (reserved.HasError()) {
            return Result<void>::Failure(reserved.ErrorValue());
        }
        ProjectSessionActivationLease activation = std::move(reserved).Value();
        const std::filesystem::path &projectRoot = activation.Candidate().projectRoot;

        if (auto initialized = InitializeController(projectRoot); initialized.HasError())
            return initialized;
        sourceCommands_.BeginSession();
        snackbarHost_ = std::make_unique<EditorSnackbarHost>(controller_->DataBus());
        host_.SetCurrentProjectRoot(controller_->ViewModel().projectRoot);
        LoadProjectInputProfile(controller_->ViewModel().projectRoot);
        RestoreExtensionWorkspace();
        viewportSceneState_.Replace(controller_->ViewportScene());
        publishedRevisions_.scene = controller_->ViewportScene().documentRevision;
        publishedRevisions_.selection = controller_->CurrentSelectionRevision();
        publishedRevisions_.viewport = controller_->CurrentViewportRevision();
        publishedRevisions_.viewportScene = controller_->CurrentViewportSceneRevision();
        LOG_INFO("editor.workspace", "EditorWorkspaceScreen entered for '%s'", controller_->ViewModel().projectRoot.c_str());

        AttachWorkspacePanels();
        UpdateStatusItems();
        if (auto committed = activation.Commit(); committed.HasError()) {
            contributions_.panels.DetachAll();
            viewportSceneState_.Clear();
            controller_.reset();
            return committed;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::NavigationSettings */
    Result<NavigationProjectSettingsSession *> EditorWorkspaceScreen::NavigationSettings() {
        return controller_ ? controller_->NavigationSettings() : Result<NavigationProjectSettingsSession *>::Success(nullptr);
    }

    /** @copydoc EditorWorkspaceScreen::OnUpdate */
    void EditorWorkspaceScreen::OnUpdate(float dt) {
        if (controller_) {
            while (const auto destination = view_.UpdateExtensionActivities())
                controller_->ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                             .targetIndex = static_cast<int>(*destination),
                                             .stringPayload = std::string{}});
            controller_->ReconcileAudioPreview();
            controller_->UpdatePlayPresentation(dt);
            if (packages_.pump) {
                const auto &model = controller_->ViewModel();
                const auto boundary = model.playState == EditorPlayState::Idle || model.playState == EditorPlayState::Failed
                                          ? Packages::PackageActivationBoundary::Quiescent
                                          : Packages::PackageActivationBoundary::Running;
                static_cast<void>(packages_.pump(std::filesystem::path{model.projectRoot}, boundary));
            }
            controller_->UpdateNavigationSettingsProjection();
            controller_->UpdateAutosave(dt, settings_.Snapshot().settings.autoSaveIntervalMinutes);
            controller_->UpdateExternalSceneWatch(dt);
            controller_->UpdateGameplaySources(dt);
            if (projectServices_.assetRegistry != nullptr) {
                controller_->RefreshAssets(projectServices_.assetRegistry->Snapshot());
            }
            controller_->UpdateContentBrowser();
            controller_->SynchronizeRuntimeScenePreview();
            controller_->UpdateFps(ImGui::GetIO().Framerate);
        }
    }

    /** @copydoc EditorWorkspaceScreen::OnInputSnapshot */
    void EditorWorkspaceScreen::OnInputSnapshot() {
        if (!controller_ || (controller_->ViewModel().playState != EditorPlayState::Playing &&
                             controller_->ViewModel().playState != EditorPlayState::Paused)) {
            input_.gameplayFrames.Reset();
            return;
        }
        input_.gameplayFrames.Capture(input_.router, input_.context);
    }

    /** @copydoc EditorWorkspaceScreen::OnFixedUpdate */
    void EditorWorkspaceScreen::OnFixedUpdate(const std::uint64_t simulationTick, const double fixedDeltaSeconds) {
        if (!controller_ || (controller_->ViewModel().playState != EditorPlayState::Playing &&
                             controller_->ViewModel().playState != EditorPlayState::Paused)) {
            return;
        }
        const Input::GameplayInputFrame command = input_.gameplayFrames.Consume(simulationTick);
        const Gameplay::GameplayInputAction action{Gameplay::GameplayActionId{kGameplayMoveAction},
                                                   command.moveX,
                                                   command.moveY,
                                                   command.moveDown,
                                                   command.movePressed,
                                                   command.moveReleased};
        controller_->UpdatePlayFixed({&action, 1}, fixedDeltaSeconds);
        PublishViewportSceneIfChanged();
    }

    /** @copydoc EditorWorkspaceScreen::Draw */
    void EditorWorkspaceScreen::Draw(const GuiContentRegion &contentRegion) {
        if (!controller_) {
            return;
        }

        using enum EditorWorkspaceViewCommand;
        EditorWorkspaceViewCommandData command;
        view_.Draw(controller_->ViewModel(), command, contentRegion);

        if (command.command == None && !command.menuInvocation.has_value()) {
            sourceCommands_.RouteInput(input_.router, input_.context, *controller_, command);
        }

        if (command.menuInvocation.has_value()) {
            host_.DispatchMenuInvocation(*command.menuInvocation);
        }

        if (command.command != None) {
            if (command.command == OpenPlayTopologyProfiles) {
                OpenPlayTopologyProfilesModal();
            } else if (command.command == CompareExternalScene) {
                OpenSceneComparison();
            } else if (command.command == CreateLuaBehavior || command.command == CreateNativeBehavior) {
                using enum GameplayBehaviorKind;
                const GameplayBehaviorKind kind = command.command == CreateNativeBehavior ? Native : Lua;
                const std::string requestedDirectory =
                    command.stringPayload.value_or(controller_->ViewModel().contentBrowser.absoluteCurrentPath);
                OpenGameplayBehaviorModal(kind, requestedDirectory);
            } else if (command.command == CloseWorkspaceDocument &&
                       sourceCommands_.OpenCloseModal(*controller_, command, modalHost_, context_)) {
                // The exclusive modal owns only the decision; the controller owns the document transaction.
            } else {
                controller_->ProcessCommand(command);
            }
            if (command.command == ReturnToWelcome) {
                static_cast<void>(host_.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}));
            }
        }
        PublishViewportSceneIfChanged();

        if (snackbarHost_) {
            const std::optional<SnackbarActionInvokedEvent> action = snackbarHost_->Draw(context_, ImGui::GetIO().DeltaTime);
            if (action.has_value() && action->actionId == "open_logs") {
                controller_->ProcessCommand(
                    EditorWorkspaceViewCommandData{.command = ChangeActivePanel, .stringPayload = "horo.global_dock"});
            }
        }

        UpdateStatusItems();
    }

    /** @copydoc EditorWorkspaceScreen::CollectActivePanelIds */
    void EditorWorkspaceScreen::CollectActivePanelIds(std::vector<std::string_view> &output) const {
        if (!controller_) {
            return;
        }
        const EditorWorkspaceViewModel &viewModel = controller_->ViewModel();
        const auto append = [&output](const std::string &panelId) {
            if (!panelId.empty()) {
                output.push_back(panelId);
            }
        };
        append(viewModel.activeLeftPanelId);
        append(viewModel.activeRightPanelId);
        append(viewModel.activeLeftTopPanelId);
        append(viewModel.activeLeftBottomPanelId);
        append(viewModel.activeRightTopPanelId);
        append(viewModel.activeRightBottomPanelId);
        append(viewModel.activeBottomPanelId);
        append(viewModel.activeBottomLeftPanelId);
        append(viewModel.activeBottomRightPanelId);
        append(viewModel.activeDocumentPanelId);
    }

    /** @copydoc EditorWorkspaceScreen::CollisionSettings */
    Result<std::shared_ptr<CollisionProjectSettingsSession>> EditorWorkspaceScreen::CollisionSettings() {
        return controller_ ? controller_->CollisionSettings() : GuiScreen::CollisionSettings();
    }

    /** @copydoc WorkspaceSourceDocumentCommands::BeginSession */
    void WorkspaceSourceDocumentCommands::BeginSession() {
        closeLifetime_ = std::make_shared<int>(0);
    }

    /** @copydoc WorkspaceSourceDocumentCommands::EndSession */
    void WorkspaceSourceDocumentCommands::EndSession() {
        closeLifetime_.reset();
    }

    /** @copydoc WorkspaceSourceDocumentCommands::RouteInput */
    void WorkspaceSourceDocumentCommands::RouteInput(Input::InputRouter &router, const Input::InputContextToken &inputContext,
                                                     EditorWorkspaceController &controller, EditorWorkspaceViewCommandData &command) {
        RouteWorkspaceInputAction(router, inputContext, controller.ViewModel(), command, controller.HasActivePCGDocument(),
                                  {ImGui::GetIO().WantTextInput,
                                   ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)});
    }

    /** @copydoc EditorWorkspaceScreen::PublishViewportSceneIfChanged */
    void EditorWorkspaceScreen::PublishViewportSceneIfChanged() {
        if (!controller_) {
            return;
        }
        const DocumentRevision documentRevision = controller_->ViewportScene().documentRevision;
        const SelectionRevision selectionRevision = controller_->CurrentSelectionRevision();
        const ViewportRevision viewportRevision = controller_->CurrentViewportRevision();
        const std::uint64_t viewportSceneRevision = controller_->CurrentViewportSceneRevision();
        if (documentRevision == publishedRevisions_.scene && selectionRevision == publishedRevisions_.selection &&
            viewportRevision == publishedRevisions_.viewport && viewportSceneRevision == publishedRevisions_.viewportScene) {
            return;
        }
        viewportSceneState_.Replace(controller_->ViewportScene());
        publishedRevisions_.scene = documentRevision;
        publishedRevisions_.selection = selectionRevision;
        publishedRevisions_.viewport = viewportRevision;
        publishedRevisions_.viewportScene = viewportSceneRevision;
    }

    /** @copydoc EditorWorkspaceScreen::UpdateStatusItems */
    void EditorWorkspaceScreen::UpdateStatusItems() {
        if (!controller_) {
            return;
        }
        const EditorWorkspaceViewModel &viewModel = controller_->ViewModel();
        static_cast<void>(
            contributions_.statusItems
                .Update("horo.status.document",
                        EditorStatusItemContent{.iconResourceId = "horo.status.document",
                                                .label = context_.localization.Get("editor", viewModel.isDirty ? "status.document.unsaved"
                                                                                                               : "status.document.saved"),
                                                .tone = viewModel.isDirty ? EditorStatusItemTone::Warning : EditorStatusItemTone::Success,
                                                .available = true}));
        static_cast<void>(
            contributions_.statusItems
                .Update("horo.status.selection",
                        EditorStatusItemContent{.value = context_.localization.Get("editor", viewModel.primarySelection.has_value()
                                                                                                 ? "status.selection.one"
                                                                                                 : "status.selection.none"),
                                                .available = true}));
    }

}  // namespace Horo::Editor::WorkspaceScreenDetail

namespace Horo::Editor {
    void RegisterEditorWorkspaceScreen(ScreenRegistry &registry, PackageWorkspaceAccess packages) {
        registry.Register(GuiRouteKind::EditorWorkspace,
                          [packages = std::move(packages)](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<WorkspaceScreenDetail::EditorWorkspaceScreen>(services, packages);
        });
    }
}  // namespace Horo::Editor
