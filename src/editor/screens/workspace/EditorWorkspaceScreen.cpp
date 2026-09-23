#include "EditorWorkspaceView.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorServiceRegistry.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorSnackbarHost.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/PathUtils.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/input/EditorInputActions.h"
#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.h"
#include "editor/modals/scene_compare/SceneConflictCompareModal.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/NavigationErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <filesystem>
#include <format>
#include <imgui.h>
#include <memory>
#include <optional>
#include <portable-file-dialogs.h>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    namespace {
        struct WorkspacePanelServices {
            IEditorGuiRenderer *guiRenderer{nullptr};
            IEditorViewportRenderer *viewportRenderer{nullptr};
            const Log::IStructuredLogQuery *logQuery{nullptr};
            const IBuildOutputQuery *buildOutputQuery{nullptr};
            const IOperationQuery *operationQuery{nullptr};
            IOperationControl *operationControl{nullptr};
        };

        struct WorkspaceProjectServices {
            Assets::AssetRegistry *assetRegistry{nullptr};
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

        [[nodiscard]] std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
            if (error) {
                return {};
            }
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            return error ? absolute : canonical;
        }

        [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
        }

        [[nodiscard]] std::optional<std::filesystem::path> ResolveGameplayBehaviorDestination(const std::filesystem::path &projectRoot,
                                                                                              const GameplayBehaviorKind kind,
                                                                                              const std::string &requestedDirectory) {
            using enum GameplayBehaviorKind;
            const std::filesystem::path normalizedProjectRoot = NormalizeAbsolutePath(projectRoot);
            const std::filesystem::path assetsRoot = normalizedProjectRoot / "assets";
            const std::filesystem::path scriptsRoot = assetsRoot / "scripts";
            const std::filesystem::path requested = NormalizeAbsolutePath(requestedDirectory);
            std::filesystem::path destination;
            if (kind == Native) {
                destination = normalizedProjectRoot / "source" / "gameplay";
            } else if (HasPathPrefix(scriptsRoot, requested)) {
                destination = requested;
            } else {
                destination = scriptsRoot;
            }

            if ((kind == Native && !HasPathPrefix(normalizedProjectRoot, destination)) ||
                (kind == Lua && !HasPathPrefix(assetsRoot, destination))) {
                return std::nullopt;
            }
            return destination;
        }

        [[nodiscard]] std::string SuggestGameplayBehaviorBaseName(const std::filesystem::path &destination,
                                                                  const GameplayBehaviorKind kind) {
            using enum GameplayBehaviorKind;
            const std::string extension = kind == Native ? ".cpp" : ".horo_script";
            std::error_code error;
            for (std::size_t suffix = 0; suffix < 1000; ++suffix) {
                if (const std::string stem = suffix == 0 ? "NewBehavior" : std::format("NewBehavior{}", suffix + 1);
                    !std::filesystem::exists(destination / (stem + extension), error)) {
                    return stem;
                }
                if (error) {
                    break;
                }
            }
            return "NewBehavior";
        }

        class EditorWorkspaceScreen final : public GuiScreen {
        public:
            explicit EditorWorkspaceScreen(const EditorServiceRegistry &services)
                : host_(services.Get<GuiScreenHost>()), services_(services), modalHost_(services.Get<EditorModalHost>()),
                  context_(services.GetConst<EditorGuiContext>()), registry_(services.Get<WorkspacePanelRegistry>()),
                  statusItems_(services.Get<EditorStatusItemRegistry>()), inputRouter_(services.Get<Input::InputRouter>()),
                  workspaceInputContext_(
                      inputRouter_.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace)),
                  view_(context_, registry_, services.Get<std::uintptr_t>(), inputRouter_, workspaceInputContext_),
                  viewportSceneState_(services.Get<EditorViewportSceneState>()),
                  runtimeScene_(services.Get<Runtime::RuntimeSceneService>()), settings_(services.Get<EditorSettingsService>()),
                  projectOpenService_(services.Get<ProjectOpenService>()),
                  panelServices_{
                      .guiRenderer = services.TryGet<IEditorGuiRenderer>(),
                      .viewportRenderer = services.TryGet<IEditorViewportRenderer>(),
                      .logQuery = services.TryGetConst<Log::IStructuredLogQuery>(),
                      .buildOutputQuery = services.TryGetConst<IBuildOutputQuery>(),
                      .operationQuery = services.TryGetConst<IOperationQuery>(),
                      .operationControl = services.TryGet<IOperationControl>(),
                  },
                  projectServices_{
                      .assetRegistry = services.TryGet<Assets::AssetRegistry>(),
                      .importerCatalog = services.TryGetConst<Assets::AssetImporterCatalogSnapshot>(),
                      .mutations = services.TryGet<ProjectMutationCoordinator>(),
                      .durableFiles = services.TryGet<DurableFileSystem>(),
                  } {}

            ScreenId Id() const override {
                return static_cast<ScreenId>(GuiRouteKind::EditorWorkspace);
            }

            Result<void> OnEnter(const GuiRoute &route) override {
                if (!std::holds_alternative<EditorWorkspaceRouteParameters>(route.parameters)) {
                    return Result<void>::Failure(MakeError(NavigationErrors::InvalidRouteParameters));
                }
                const auto &params = std::get<EditorWorkspaceRouteParameters>(route.parameters);
                auto reserved = projectOpenService_.ReserveSession(params.session);
                if (reserved.HasError()) {
                    return Result<void>::Failure(reserved.ErrorValue());
                }
                ProjectSessionActivationLease activation = std::move(reserved).Value();
                std::string projectRoot = activation.Candidate().projectRoot.string();

                const Assets::AssetRegistrySnapshot assetSnapshot =
                    projectServices_.assetRegistry ? projectServices_.assetRegistry->Snapshot() : Assets::AssetRegistrySnapshot{};
                const Application::GameplayBuildEnvironment *gameplayEnvironment =
                    services_.TryGet<Application::GameplayBuildEnvironment>();
                controller_ =
                    std::make_unique<EditorWorkspaceController>(projectRoot, runtimeScene_, assetSnapshot,
                                                                EditorWorkspaceDependencies{
                                                                    .mutableAssetRegistry = projectServices_.assetRegistry,
                                                                    .mutations = projectServices_.mutations,
                                                                    .durableFiles = projectServices_.durableFiles,
                                                                    .importerCatalog = projectServices_.importerCatalog,
                                                                    .jobs = &services_.Get<JobSystem>(),
                                                                    .gameplayBuilds = services_.TryGet<Application::GameplayBuildService>(),
                                                                    .gameplayBuildEnvironment =
                                                                        gameplayEnvironment != nullptr
                                                                            ? *gameplayEnvironment
                                                                            : Application::GameplayBuildEnvironment{},
                                                                    .localization = &context_.localization,
                                                                    .engineEvents = &context_.engineEvents,
                                                                });
                if (controller_->InitializationError().has_value()) {
                    const Error error = *controller_->InitializationError();
                    controller_.reset();
                    return Result<void>::Failure(error);
                }
                snackbarHost_ = std::make_unique<EditorSnackbarHost>(controller_->DataBus());
                host_.SetCurrentProjectRoot(controller_->ViewModel().projectRoot);
                LoadProjectInputProfile(controller_->ViewModel().projectRoot);
                viewportSceneState_.Replace(controller_->ViewportScene());
                publishedRevisions_.scene = controller_->ViewportScene().documentRevision;
                publishedRevisions_.selection = controller_->CurrentSelectionRevision();
                publishedRevisions_.viewport = controller_->CurrentViewportRevision();
                publishedRevisions_.viewportScene = controller_->CurrentViewportSceneRevision();
                LOG_INFO("editor.workspace", "EditorWorkspaceScreen entered for '%s'", controller_->ViewModel().projectRoot.c_str());

                PanelContext panelContext{
                    .dataBus = controller_->DataBus(),
                    .guiRenderer = panelServices_.guiRenderer,
                    .viewportRenderer = panelServices_.viewportRenderer,
                    .inputRouter = &inputRouter_,
                    .workspaceInputContext = &workspaceInputContext_,
                    .logQuery = panelServices_.logQuery,
                    .buildOutputQuery = panelServices_.buildOutputQuery,
                    .operationQuery = panelServices_.operationQuery,
                    .operationControl = panelServices_.operationControl,
                    // Built-in panels have no extension activation identity; extension hosts inject
                    // their own provider-owned context at the descriptor activation boundary.
                    .surfaceEvents = nullptr,
                };
                registry_.AttachAll(panelContext);
                UpdateStatusItems();
                if (auto committed = activation.Commit(); committed.HasError()) {
                    registry_.DetachAll();
                    viewportSceneState_.Clear();
                    controller_.reset();
                    return committed;
                }
                return Result<void>::Success();
            }

            void OnUpdate(float dt) override {
                if (controller_) {
                    controller_->UpdatePlayPresentation(dt);
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

            void OnFixedUpdate(const double fixedDeltaSeconds) override {
                if (!controller_ || (controller_->ViewModel().playState != EditorPlayState::Playing &&
                                     controller_->ViewModel().playState != EditorPlayState::Paused)) {
                    return;
                }
                const Input::ActionValue move = inputRouter_.ReadAction(workspaceInputContext_, Input::ActionId{kGameplayMoveAction});
                const Gameplay::GameplayInputAction action{Gameplay::GameplayActionId{kGameplayMoveAction},
                                                           move.x,
                                                           move.y,
                                                           move.down,
                                                           move.pressed,
                                                           move.released};
                controller_->UpdatePlayFixed({&action, 1}, fixedDeltaSeconds);
                PublishViewportSceneIfChanged();
            }

            void Draw(const GuiContentRegion &contentRegion) override {
                if (!controller_) {
                    return;
                }

                using enum EditorWorkspaceViewCommand;
                EditorWorkspaceViewCommandData command;
                view_.Draw(controller_->ViewModel(), command, contentRegion);

                if (command.command == None && !command.menuInvocation.has_value()) {
                    RouteInputAction(command);
                }

                if (command.menuInvocation.has_value()) {
                    host_.DispatchMenuInvocation(*command.menuInvocation);
                }

                if (command.command != None) {
                    if (command.command == CompareExternalScene) {
                        OpenSceneComparison();
                    } else if (command.command == CreateLuaBehavior || command.command == CreateNativeBehavior) {
                        using enum GameplayBehaviorKind;
                        const GameplayBehaviorKind kind = command.command == CreateNativeBehavior ? Native : Lua;
                        const std::string requestedDirectory =
                            command.stringPayload.value_or(controller_->ViewModel().contentBrowser.absoluteCurrentPath);
                        OpenGameplayBehaviorModal(kind, requestedDirectory);
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

            void CollectActivePanelIds(std::vector<std::string_view> &output) const override {
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

            bool HandleMenuInvocation(const EditorMenuInvocation &invocation) override {
                EditorMenuAction action = invocation.action;
                if (controller_ && action == EditorMenuAction::SaveScene && !controller_->CurrentScenePath().has_value())
                    action = EditorMenuAction::SaveSceneAs;
                if (controller_ && action == EditorMenuAction::CreatePrimitive && invocation.primitive.has_value()) {
                    EditorWorkspaceViewCommandData command;
                    command.command = EditorWorkspaceViewCommand::CreatePrimitive;
                    command.primitivePayload = invocation.primitive;
                    command.objectPayload = controller_->ViewModel().primarySelection;
                    controller_->ProcessCommand(command);
                    PublishViewportSceneIfChanged();
                    return true;
                }
                if (controller_ && (action == EditorMenuAction::SaveSceneAs || action == EditorMenuAction::SaveSceneCopyAs)) {
                    const auto &currentPath = controller_->CurrentScenePath();
                    const std::filesystem::path projectRoot{controller_->ViewModel().projectRoot};
                    const std::filesystem::path suggestedPath = currentPath.value_or(projectRoot / "assets" / "scenes" / "main.horo");

                    auto nativeDialogContext = inputRouter_.PushContext(Input::InputContextId{"editor.native_dialog.scene_save"},
                                                                        Input::InputContextKind::NativeDialog);
                    const bool copyOnly = action == EditorMenuAction::SaveSceneCopyAs;
#if defined(__APPLE__)
                    // portable-file-dialogs forwards this value to AppleScript's
                    // `default name`, where a full path is interpreted as a literal
                    // filename and its separators become colons.
                    const std::string dialogDefault = suggestedPath.filename().string();
#else
                    const std::string dialogDefault = suggestedPath.string();
#endif
                    pfd::save_file dialog(std::string{context_.localization.Get("editor", copyOnly ? "workspace.scene_save_copy_as.title"
                                                                                                   : "workspace.scene_save_as.title")},
                                          dialogDefault,
                                          {std::string{context_.localization.Get("editor", "workspace.scene_save.file_type")}, "*.horo"});
                    const std::string selected = dialog.result();
                    if (selected.empty()) {
                        return true;
                    }

                    std::filesystem::path destination{selected};
                    if (destination.extension().empty()) {
                        destination += ".horo";
                    }
                    if (!destination.is_absolute()) {
                        LOG_ERROR("editor.scene_document", "Native scene destination dialog returned a non-absolute path '%s'.",
                                  destination.string().c_str());
                        return true;
                    }
                    std::error_code canonicalError;
                    const std::filesystem::path normalized = std::filesystem::weakly_canonical(destination, canonicalError);
                    if (canonicalError) {
                        LOG_ERROR("editor.scene_document", "Scene destination '%s' could not be normalized: %s",
                                  destination.string().c_str(), canonicalError.message().c_str());
                        return true;
                    }

                    EditorWorkspaceViewCommandData command;
                    command.command = copyOnly ? EditorWorkspaceViewCommand::SaveSceneCopyAs : EditorWorkspaceViewCommand::SaveSceneAs;
                    command.stringPayload = normalized.string();
                    controller_->ProcessCommand(command);
                    PublishViewportSceneIfChanged();
                    return true;
                }
                if (!controller_ ||
                    (action != EditorMenuAction::SaveScene && action != EditorMenuAction::Undo && action != EditorMenuAction::Redo)) {
                    return false;
                }

                using enum EditorMenuAction;
                EditorWorkspaceViewCommandData command;
                if (action == SaveScene) {
                    command.command = EditorWorkspaceViewCommand::SaveScene;
                } else if (action == Undo) {
                    command.command = EditorWorkspaceViewCommand::UndoScene;
                } else {
                    command.command = EditorWorkspaceViewCommand::RedoScene;
                }
                controller_->ProcessCommand(command);
                PublishViewportSceneIfChanged();
                return true;
            }

            LeaveDecision CanLeave(const LeaveTarget &) const override {
                using enum LeaveAction;
                using enum LeaveDisposition;
                if (controller_ && controller_->ViewModel().isDirty) {
                    LeaveRequirement requirement{.kind = LeaveRequirementKind::DirtyDocument,
                                                 .subject = 1,
                                                 .revision = 1,
                                                 .allowedActions = {Save, Discard, Stay}};
                    return LeaveDecision{.disposition = RequireResolution, .requirement = requirement};
                }
                return LeaveDecision{.disposition = Allow, .requirement = std::nullopt};
            }

            Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) override {
                using enum LeaveAction;
                using enum LeaveDisposition;
                if (resolution.subject != 1 || resolution.revision != 1) {
                    return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                }
                if (resolution.action == Save) {
                    if (controller_) {
                        EditorWorkspaceViewCommandData command;
                        command.command = EditorWorkspaceViewCommand::SaveScene;
                        controller_->ProcessCommand(command);
                        if (controller_->ViewModel().isDirty) {
                            return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Deny, .requirement = std::nullopt});
                        }
                    }
                    return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Allow, .requirement = std::nullopt});
                }
                if (resolution.action == Discard) {
                    if (controller_) {
                        EditorWorkspaceViewCommandData command;
                        command.command = EditorWorkspaceViewCommand::DiscardSceneRecovery;
                        controller_->ProcessCommand(command);
                    }
                    return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Allow, .requirement = std::nullopt});
                }
                if (resolution.action == Stay) {
                    return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Deny, .requirement = std::nullopt});
                }
                return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceLeaveActionNotAllowed));
            }

            void OnLeave() override {
                LOG_INFO("editor.workspace", "EditorWorkspaceScreen leaving.");
                if (controller_) {
                    controller_->FlushAutosave();
                }
                static_cast<void>(statusItems_.Update("horo.status.document", EditorStatusItemContent{.available = false}));
                static_cast<void>(statusItems_.Update("horo.status.selection", EditorStatusItemContent{.available = false}));
                registry_.DetachAll();
                if (previousInputProfile_.has_value()) {
                    if (const Result<void> restored = inputRouter_.SetProfile(std::move(*previousInputProfile_)); restored.HasError()) {
                        LOG_ERROR("editor.input", "Unable to restore editor input profile: %s", restored.ErrorValue().message.c_str());
                    }
                    previousInputProfile_.reset();
                }
                workspaceInputContext_.Reset();
                viewportSceneState_.Clear();
                publishedRevisions_ = {};
                controller_.reset();
            }

        private:
            void OpenSceneComparison() {
                Result<SceneDocumentComparisonRequest> captured = controller_->CaptureExternalSceneComparison();
                if (captured.HasError()) {
                    LOG_ERROR("editor.scene_document", "External scene comparison failed: %s", captured.ErrorValue().message.c_str());
                    return;
                }
                Result<void> opened = modalHost_.OpenRoot(
                    std::make_unique<SceneConflictCompareModal>(context_, services_.Get<JobSystem>(), std::move(captured).Value()));
                if (opened.HasError()) {
                    LOG_WARN("editor.scene_document", "External scene comparison modal could not open: %s",
                             opened.ErrorValue().message.c_str());
                }
            }

            void OpenGameplayBehaviorModal(const GameplayBehaviorKind kind, const std::string &requestedDirectory) {
                if (!controller_) {
                    return;
                }
                const std::optional<std::filesystem::path> destination =
                    ResolveGameplayBehaviorDestination(controller_->ViewModel().projectRoot, kind, requestedDirectory);
                if (!destination.has_value()) {
                    EditorWorkspaceViewCommandData fallback;
                    fallback.command = kind == GameplayBehaviorKind::Native ? EditorWorkspaceViewCommand::CreateNativeBehavior
                                                                            : EditorWorkspaceViewCommand::CreateLuaBehavior;
                    fallback.stringPayload = requestedDirectory;
                    controller_->ProcessCommand(fallback);
                    return;
                }

                const std::string baseName = SuggestGameplayBehaviorBaseName(*destination, kind);
                Result<void> opened =
                    modalHost_.OpenRoot(std::make_unique<GameplayBehaviorFilenameModal>(context_, kind, destination->string(), baseName,
                                                                                        [this](CreateGameplayBehaviorRequest request) {
                    if (!controller_) {
                        return;
                    }
                    EditorWorkspaceViewCommandData create;
                    create.command = request.kind == GameplayBehaviorKind::Native ? EditorWorkspaceViewCommand::CreateNativeBehavior
                                                                                  : EditorWorkspaceViewCommand::CreateLuaBehavior;
                    create.gameplayBehaviorRequest = std::move(request);
                    controller_->ProcessCommand(create);
                    PublishViewportSceneIfChanged();
                }));
                if (opened.HasError()) {
                    LOG_WARN("editor.asset_actions", "Gameplay behavior filename modal could not open: %s",
                             opened.ErrorValue().message.c_str());
                }
            }

            void LoadProjectInputProfile(const std::filesystem::path &projectRoot) {
                previousInputProfile_ = inputRouter_.Profile();
                Input::InputBindingProfile merged{.profileId = "project-composed"};
                const auto mergeProfile = [&](const Input::InputBindingProfile &profile, const std::string &source) {
                    Result<Input::InputBindingProfile> layered = Input::MergeBindingProfiles(merged, profile);
                    if (layered.HasError()) {
                        LOG_ERROR("editor.input", "Unable to layer input profile '%s': %s", source.c_str(),
                                  layered.ErrorValue().message.c_str());
                        return false;
                    }
                    merged = std::move(layered).Value();
                    return true;
                };
                const auto mergeFile = [&](const std::filesystem::path &path) {
                    if (std::error_code error; !std::filesystem::exists(path, error) || error) {
                        return true;
                    }
                    const Result<Input::InputBindingProfile> loaded = Input::LoadBindingProfile(path);
                    if (loaded.HasError()) {
                        LOG_ERROR("editor.input", "Keeping last valid input profile; '%s' is invalid: %s", path.string().c_str(),
                                  loaded.ErrorValue().message.c_str());
                        return false;
                    }
                    return mergeProfile(loaded.Value(), path.string());
                };

                // Defaults are resolved by the action descriptors. Profile layers then
                // apply from project defaults to user-wide and project-user overrides.
                if (!mergeFile(projectRoot / ".horo" / "input.json") || !mergeProfile(*previousInputProfile_, "editor-global")) {
                    return;
                }
                if (const Result<Application::ProjectMetadata> metadata = Application::LoadProjectMetadata(projectRoot);
                    metadata.HasValue() && !mergeFile(ResolveEditorSettingsHomeDirectory() / ".horo" / "input" / "projects" /
                                                      (metadata.Value().projectId + ".json"))) {
                    return;
                }

                const Result<void> applied = inputRouter_.SetProfile(std::move(merged));
                if (applied.HasError()) {
                    LOG_ERROR("editor.input", "Keeping last valid input profile for project '%s': %s", projectRoot.string().c_str(),
                              applied.ErrorValue().message.c_str());
                }
            }

            void RouteInputAction(EditorWorkspaceViewCommandData &command) {
                const auto pressed = [this](const char *id) {
                    return inputRouter_.ReadAction(workspaceInputContext_, Input::ActionId{id}).pressed;
                };
                if (pressed(kActionRedo)) {
                    command.command = EditorWorkspaceViewCommand::RedoScene;
                } else if (pressed(kActionUndo)) {
                    command.command = EditorWorkspaceViewCommand::UndoScene;
                } else if (pressed(kActionSave)) {
                    command.command = EditorWorkspaceViewCommand::SaveScene;
                } else if (pressed(kActionToolSelect)) {
                    command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
                    command.transformToolPayload = EditorTransformTool::Select;
                } else if (pressed(kActionToolMove)) {
                    command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
                    command.transformToolPayload = EditorTransformTool::Move;
                } else if (pressed(kActionToolRotate)) {
                    command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
                    command.transformToolPayload = EditorTransformTool::Rotate;
                } else if (pressed(kActionToolScale)) {
                    command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
                    command.transformToolPayload = EditorTransformTool::Scale;
                } else if (controller_->ViewModel().primarySelection.has_value() && pressed(kActionDuplicate)) {
                    command.command = EditorWorkspaceViewCommand::DuplicateObject;
                    command.objectPayload = controller_->ViewModel().primarySelection;
                } else if (controller_->ViewModel().primarySelection.has_value() && pressed(kActionDelete)) {
                    command.command = EditorWorkspaceViewCommand::DeleteObject;
                    command.objectPayload = controller_->ViewModel().primarySelection;
                }
            }

            /** @brief Publishes document, selection, or viewport changes to the composition-owned render
             * state. */
            void PublishViewportSceneIfChanged() {
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

            void UpdateStatusItems() {
                if (!controller_) {
                    return;
                }
                const EditorWorkspaceViewModel &viewModel = controller_->ViewModel();
                static_cast<void>(
                    statusItems_.Update("horo.status.document",
                                        EditorStatusItemContent{.iconResourceId = "horo.status.document",
                                                                .label = context_.localization.Get("editor", viewModel.isDirty
                                                                                                                 ? "status.document.unsaved"
                                                                                                                 : "status.document.saved"),
                                                                .tone = viewModel.isDirty ? EditorStatusItemTone::Warning
                                                                                          : EditorStatusItemTone::Success,
                                                                .available = true}));
                static_cast<void>(
                    statusItems_.Update("horo.status.selection",
                                        EditorStatusItemContent{.value = context_.localization.Get("editor",
                                                                                                   viewModel.primarySelection.has_value()
                                                                                                       ? "status.selection.one"
                                                                                                       : "status.selection.none"),
                                                                .available = true}));
            }

            GuiScreenHost &host_;
            const EditorServiceRegistry &services_;
            EditorModalHost &modalHost_;
            const EditorGuiContext &context_;
            WorkspacePanelRegistry &registry_;
            EditorStatusItemRegistry &statusItems_;
            Input::InputRouter &inputRouter_;
            Input::InputContextToken workspaceInputContext_;
            EditorWorkspaceView view_;
            EditorViewportSceneState &viewportSceneState_;
            Runtime::RuntimeSceneService &runtimeScene_;
            EditorSettingsService &settings_;
            ProjectOpenService &projectOpenService_;
            WorkspacePanelServices panelServices_{};
            WorkspaceProjectServices projectServices_{};
            WorkspacePublishedRevisions publishedRevisions_{};
            std::unique_ptr<EditorWorkspaceController> controller_;
            std::unique_ptr<EditorSnackbarHost> snackbarHost_;
            std::optional<Input::InputBindingProfile> previousInputProfile_;
        };
    }  // namespace

    void RegisterEditorWorkspaceScreen(ScreenRegistry &registry) {
        registry.Register(GuiRouteKind::EditorWorkspace, [](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<EditorWorkspaceScreen>(services);
        });
    }
}  // namespace Horo::Editor
