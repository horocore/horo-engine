#include "EditorWorkspaceView.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorServiceRegistry.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorSnackbarHost.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/NativeAssetRegistrySource.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/input/EditorInputActions.h"
#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.h"
#include "editor/modals/packages/PackageModal.h"
#include "editor/modals/play_topology/PlayTopologyModal.h"
#include "editor/modals/scene_compare/SceneConflictCompareModal.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/NavigationErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <array>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <memory>
#include <optional>
#include <portable-file-dialogs.h>
#include <span>
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
            const Assets::AssetImporterCatalogSnapshot *importerCatalog{nullptr};
            ProjectMutationCoordinator *mutations{nullptr};
            DurableFileSystem *durableFiles{nullptr};
        };

        /** @brief Composes the explicit project loader and retains its cooperative lease through cache publication. */
        PrefabHierarchySourceCapture PrefabCapture(const std::filesystem::path &root, const ProjectMutationCoordinator *mutations) {
            return [root, mutations](const Assets::AssetRegistrySnapshot &registry, const std::span<const Assets::AssetId> roots,
                                     const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation) {
                if (mutations == nullptr)
                    return Result<PreparedPrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                auto lease = mutations->TryAcquire({root, ProjectMutationOwner::Asset, "prefab.hierarchy.capture"});
                if (lease.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(lease.ErrorValue());
                auto project = Application::LoadProjectSourceDocument(root);
                if (project.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(project.ErrorValue());
                auto sources =
                    CapturePrefabHierarchySources(root, registry, roots, limits, project.Value().metadata.horoVersion.value, cancellation);
                if (sources.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(sources.ErrorValue());
                return Result<PreparedPrefabHierarchySources>::Success({std::move(sources).Value(), std::move(lease).Value()});
            };
        }

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
            const std::filesystem::path assetsRoot = ProjectLayout::AssetRoot(normalizedProjectRoot);
            const std::filesystem::path scriptsRoot = ProjectLayout::ScriptsRoot(normalizedProjectRoot);
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

        /** @brief Borrowed contribution catalogs; application-owned panel/status registrations outlive the screen. */
        struct WorkspaceContributionRegistries final {
            WorkspacePanelRegistry &panels;
            EditorStatusItemRegistry &statusItems;
        };

        /** @brief Exact pending leave decision captures; borrowed collision authority never extends project lifetime. */
        struct WorkspaceLeaveCapture final {
            std::vector<PCGDocumentSnapshot> pcgDocuments;
            std::weak_ptr<CollisionProjectSettingsSession> collisionOwner;
            std::shared_ptr<const Application::ProjectCollisionSettings> collisionRoot;
        };

        class EditorWorkspaceScreen final : public GuiScreen {
        public:
            explicit EditorWorkspaceScreen(const EditorServiceRegistry &services, PackageWorkspaceAccess packages)
                : packages_(std::move(packages)), host_(services.Get<GuiScreenHost>()), services_(services),
                  modalHost_(services.Get<EditorModalHost>()), context_(services.GetConst<EditorGuiContext>()),
                  contributions_{services.Get<WorkspacePanelRegistry>(), services.Get<EditorStatusItemRegistry>()},
                  input_(services.Get<Input::InputRouter>()),
                  view_(context_, contributions_.panels, services.Get<std::uintptr_t>(), input_.router, input_.context,
                        services.TryGet<Extensions::EditorActivityHost>(), services.TryGet<IEditorGuiRenderer>()),
                  viewportSceneState_(services.Get<EditorViewportSceneState>()),
                  runtimeScene_(services.Get<Runtime::RuntimeSceneService>()), settings_(services.Get<EditorSettingsService>()),
                  projectOpenService_(services.Get<ProjectOpenService>()),
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
                const std::filesystem::path &projectRoot = activation.Candidate().projectRoot;

                if (auto initialized = InitializeController(projectRoot); initialized.HasError())
                    return initialized;
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

                PanelContext panelContext{
                    .dataBus = controller_->DataBus(),
                    .guiRenderer = panelServices_.guiRenderer,
                    .viewportRenderer = panelServices_.viewportRenderer,
                    .inputRouter = &input_.router,
                    .workspaceInputContext = &input_.context,
                    .logQuery = panelServices_.logQuery,
                    .buildOutputQuery = panelServices_.buildOutputQuery,
                    .gameplayBuilds = panelServices_.gameplayBuilds,
                    .projectRoot = controller_->ViewModel().projectRoot,
                    .operationQuery = panelServices_.operationQuery,
                    .operationControl = panelServices_.operationControl,
                    // Built-in panels have no extension activation identity; extension hosts inject
                    // their own provider-owned context at the descriptor activation boundary.
                    .surfaceEvents = nullptr,
                    .networkDebuggerQuery = panelServices_.networkDebuggerQuery,
                    .networkDebuggerControl = panelServices_.networkDebuggerControl,
                };
                contributions_.panels.AttachAll(panelContext);
                UpdateStatusItems();
                if (auto committed = activation.Commit(); committed.HasError()) {
                    contributions_.panels.DetachAll();
                    viewportSceneState_.Clear();
                    controller_.reset();
                    return committed;
                }
                return Result<void>::Success();
            }

            Result<NavigationProjectSettingsSession *> NavigationSettings() override {
                return controller_ ? controller_->NavigationSettings() : Result<NavigationProjectSettingsSession *>::Success(nullptr);
            }

            void OnUpdate(float dt) override {
                if (controller_) {
                    while (const auto destination = view_.UpdateExtensionActivities())
                        controller_->ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                                     .targetIndex = static_cast<int>(*destination),
                                                     .stringPayload = std::string{}});
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

            void OnInputSnapshot() override {
                if (!controller_ || (controller_->ViewModel().playState != EditorPlayState::Playing &&
                                     controller_->ViewModel().playState != EditorPlayState::Paused)) {
                    input_.gameplayFrames.Reset();
                    return;
                }
                input_.gameplayFrames.Capture(input_.router, input_.context);
            }

            void OnFixedUpdate(const std::uint64_t simulationTick, const double fixedDeltaSeconds) override {
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
                if (action == EditorMenuAction::ManagePackages) {
                    OpenPackagesModal();
                    return true;
                }
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
                    return SaveSceneAs(action == EditorMenuAction::SaveSceneCopyAs);
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

            Result<std::shared_ptr<CollisionProjectSettingsSession>> CollisionSettings() override {
                return controller_ ? controller_->CollisionSettings() : GuiScreen::CollisionSettings();
            }

            LeaveDecision CanLeave(const LeaveTarget &) const override {
                using enum LeaveAction;
                using enum LeaveDisposition;
                if (controller_ && (controller_->ViewModel().isDirty || controller_->HasDirtyPCGDocuments() ||
                                    controller_->HasDirtyCollisionSettings() || controller_->HasDirtyNavigationDocuments())) {
                    auto graphs = controller_->PCGDocuments().Snapshots();
                    if (graphs.HasError())
                        return LeaveDecision{.disposition = Deny, .requirement = std::nullopt};
                    leaveCapture_.pcgDocuments = std::move(graphs).Value();
                    if (const auto collision = CaptureLeaveCollision(); collision.HasError())
                        return LeaveDecision{.disposition = Deny, .requirement = std::nullopt};
                    LeaveRequirement requirement{.kind = LeaveRequirementKind::DirtyDocument,
                                                 .subject = 1,
                                                 .revision = 1,
                                                 .allowedActions = {Save, Discard, Stay}};
                    return LeaveDecision{.disposition = RequireResolution, .requirement = requirement};
                }
                return LeaveDecision{.disposition = Allow, .requirement = std::nullopt};
            }

            /** @brief Saves canonical owners in the existing order and retires them only after every required save succeeds. */
            Result<LeaveDecision> SaveForLeave() {
                using enum LeaveDisposition;
                if (!controller_)
                    return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
                if (const auto navigation = controller_->SaveNavigationDocuments(); navigation.HasError())
                    return Result<LeaveDecision>::Failure(navigation.ErrorValue());
                EditorWorkspaceViewCommandData command;
                command.command = EditorWorkspaceViewCommand::SaveScene;
                controller_->ProcessCommand(command);
                if (controller_->ViewModel().isDirty || controller_->SaveCollisionSettings().HasError() ||
                    controller_->SavePCGDocuments().HasError() || controller_->HasDirtyPCGDocuments())
                    return Result<LeaveDecision>::Success({.disposition = Deny, .requirement = std::nullopt});
                if (const auto navigation = controller_->ShutdownNavigationDocuments(); navigation.HasError())
                    return Result<LeaveDecision>::Failure(navigation.ErrorValue());
                if (const auto graphs = controller_->ShutdownPCGDocuments(); graphs.HasError())
                    return Result<LeaveDecision>::Failure(graphs.ErrorValue());
                return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
            }

            /** @brief Applies the explicit discard decision without bypassing either document group's shutdown admission. */
            Result<LeaveDecision> DiscardForLeave() {
                using enum LeaveDisposition;
                if (!controller_)
                    return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
                EditorWorkspaceViewCommandData command;
                command.command = EditorWorkspaceViewCommand::DiscardSceneRecovery;
                controller_->ProcessCommand(command);
                if (const auto collision = controller_->ShutdownCollisionSettings(true); collision.HasError())
                    return Result<LeaveDecision>::Failure(collision.ErrorValue());
                if (const auto navigation = controller_->ShutdownNavigationDocuments(true); navigation.HasError())
                    return Result<LeaveDecision>::Failure(navigation.ErrorValue());
                if (const auto graphs = controller_->ShutdownPCGDocuments(true); graphs.HasError())
                    return Result<LeaveDecision>::Failure(graphs.ErrorValue());
                return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
            }

            Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) override {
                using enum LeaveAction;
                using enum LeaveDisposition;
                if (resolution.subject != 1 || resolution.revision != 1)
                    return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                if (controller_ && resolution.action != Stay) {
                    if (const auto current = ValidateLeavePCGDocuments(); current.HasError())
                        return Result<LeaveDecision>::Failure(current.ErrorValue());
                    if (const auto current = ValidateLeaveCollision(); current.HasError())
                        return Result<LeaveDecision>::Failure(current.ErrorValue());
                }
                switch (resolution.action) {
                    case Save:
                        return SaveForLeave();
                    case Discard:
                        return DiscardForLeave();
                    case Stay:
                        return Result<LeaveDecision>::Success({.disposition = Deny, .requirement = std::nullopt});
                    default:
                        return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceLeaveActionNotAllowed));
                }
            }

            void OnLeave() override {
                LOG_INFO("editor.workspace", "EditorWorkspaceScreen leaving.");
                if (controller_) {
                    static_cast<void>(controller_->ShutdownCollisionSettings(true));
                    if (packages_.retire)
                        static_cast<void>(packages_.retire(std::filesystem::path{controller_->ViewModel().projectRoot}));
                    SaveExtensionWorkspace();
                    controller_->FlushAutosave();
                    if (const auto gameplay = controller_->ShutdownGameplayModules(); gameplay.HasError())
                        LOG_WARN("editor.gameplay", "Gameplay retirement remains owned by the application: %s",
                                 gameplay.ErrorValue().message.c_str());
                    if (const auto navigation = controller_->ShutdownNavigationDocuments(); navigation.HasError())
                        LOG_ERROR("editor.navigation_document", "Navigation document shutdown is incomplete: %s",
                                  navigation.ErrorValue().message.c_str());
                    if (const auto graphs = controller_->ShutdownPCGDocuments(); graphs.HasError())
                        LOG_ERROR("editor.pcg_document", "Graph shutdown recovery is incomplete: %s", graphs.ErrorValue().message.c_str());
                }
                static_cast<void>(contributions_.statusItems.Update("horo.status.document", EditorStatusItemContent{.available = false}));
                static_cast<void>(contributions_.statusItems.Update("horo.status.selection", EditorStatusItemContent{.available = false}));
                contributions_.panels.DetachAll();
                if (previousInputProfile_.has_value()) {
                    if (const Result<void> restored = input_.router.SetProfile(std::move(*previousInputProfile_)); restored.HasError()) {
                        LOG_ERROR("editor.input", "Unable to restore editor input profile: %s", restored.ErrorValue().message.c_str());
                    }
                    previousInputProfile_.reset();
                }
                input_.context.Reset();
                viewportSceneState_.Clear();
                publishedRevisions_ = {};
                controller_.reset();
            }

        private:
            /** @brief Requires the exact graph sessions and revisions captured by the dirty-leave prompt. */
            /** @brief Captures the cached dirty draft at the explicit leave decision, without filesystem access. */
            Result<void> CaptureLeaveCollision() const {
                leaveCapture_.collisionOwner.reset();
                leaveCapture_.collisionRoot.reset();
                if (!controller_->HasDirtyCollisionSettings())
                    return Result<void>::Success();
                auto owner = controller_->CollisionSettings();
                if (owner.HasError())
                    return Result<void>::Failure(owner.ErrorValue());
                auto root = owner.Value()->Snapshot();
                if (root.HasError())
                    return Result<void>::Failure(root.ErrorValue());
                leaveCapture_.collisionOwner = owner.Value();
                leaveCapture_.collisionRoot = std::move(root).Value();
                return Result<void>::Success();
            }

            /** @brief Refuses a delayed save/discard decision after its exact collision draft or opening changed. */
            Result<void> ValidateLeaveCollision() const {
                if (controller_->HasDirtyCollisionSettings() != static_cast<bool>(leaveCapture_.collisionRoot))
                    return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                if (!leaveCapture_.collisionRoot)
                    return Result<void>::Success();
                const auto owner = leaveCapture_.collisionOwner.lock();
                if (const auto currentOwner = controller_->CollisionSettings();
                    !owner || currentOwner.HasError() || currentOwner.Value() != owner)
                    return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                if (const auto snapshot = owner->Snapshot(); snapshot.HasError() || snapshot.Value() != leaveCapture_.collisionRoot)
                    return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                return Result<void>::Success();
            }

            Result<void> ValidateLeavePCGDocuments() const {
                const auto graphs = controller_->PCGDocuments().Snapshots();
                if (graphs.HasError())
                    return Result<void>::Failure(graphs.ErrorValue());
                if (graphs.Value().size() != leaveCapture_.pcgDocuments.size())
                    return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                for (std::size_t index = 0; index < graphs.Value().size(); ++index) {
                    const auto &current = graphs.Value()[index];
                    const auto &captured = leaveCapture_.pcgDocuments[index];
                    if (current.session != captured.session || current.revision != captured.revision ||
                        current.catalogRevision != captured.catalogRevision)
                        return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
                }
                return Result<void>::Success();
            }

            /** @brief Creates the workspace controller while the caller retains the uncommitted project activation lease. */
            Result<void> InitializeController(const std::filesystem::path &projectRoot) {
                const Assets::AssetRegistrySnapshot assetSnapshot =
                    projectServices_.assetRegistry ? projectServices_.assetRegistry->Snapshot() : Assets::AssetRegistrySnapshot{};
                const Application::GameplayBuildEnvironment *gameplayEnvironment =
                    services_.TryGet<Application::GameplayBuildEnvironment>();
                std::array<std::uint8_t, 16> sessionBytes{};
                const auto entropy = Platform::CreateNativeSecureRandomSource();
                if (const auto generated = entropy->Fill(std::as_writable_bytes(std::span(sessionBytes))); generated.HasError())
                    return Result<void>::Failure(generated.ErrorValue());
                const auto sessionOwner = Assets::AssetId::FromBytes(sessionBytes);
                if (!sessionOwner.IsValid())
                    return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Unauthorized)));
                auto registrySource = CreateNativeAssetRegistrySource(std::filesystem::path{projectRoot}, sessionOwner.ToString(), 1);
                if (registrySource.HasError())
                    return Result<void>::Failure(registrySource.ErrorValue());
                controller_ =
                    std::make_unique<EditorWorkspaceController>(projectRoot, runtimeScene_, assetSnapshot,
                                                                EditorWorkspaceDependencies{
                                                                    .mutableAssetRegistry = projectServices_.assetRegistry,
                                                                    .collision = {.inventory = projectServices_.collisionInventory,
                                                                                  .activation = projectServices_.collisionActivation},
                                                                    .mutations = projectServices_.mutations,
                                                                    .durableFiles = projectServices_.durableFiles,
                                                                    .assetRegistrySource = std::move(registrySource).Value(),
                                                                    .importerCatalog = projectServices_.importerCatalog,
                                                                    .jobs = &services_.Get<JobSystem>(),
                                                                    .gameplayBuilds = services_.TryGet<Application::GameplayBuildService>(),
                                                                    .gameplayBuildEnvironment =
                                                                        gameplayEnvironment != nullptr
                                                                            ? *gameplayEnvironment
                                                                            : Application::GameplayBuildEnvironment{},
                                                                    .localization = &context_.localization,
                                                                    .engineEvents = &context_.engineEvents,
                                                                    .networkDebugger =
                                                                        services_.TryGet<Application::NetworkDebuggerService>(),
                                                                    .documentSessionOwner = sessionOwner,
                                                                    .gameplayRetirements = host_.GameplayRetirements(),
                                                                    .prefabSources = PrefabCapture(projectRoot, projectServices_.mutations),
                                                                });
                if (controller_->InitializationError().has_value()) {
                    const Error error = *controller_->InitializationError();
                    controller_.reset();
                    return Result<void>::Failure(error);
                }
                return Result<void>::Success();
            }

            /** @brief Runs the native save dialog and command while its input context remains scoped through scene publication. */
            bool SaveSceneAs(const bool copyOnly) {
                const auto &currentPath = controller_->CurrentScenePath();
                const std::filesystem::path projectRoot{controller_->ViewModel().projectRoot};
                const std::filesystem::path suggestedPath = currentPath.value_or(ProjectLayout::ScenesRoot(projectRoot) / "main.horo");

                auto nativeDialogContext = input_.router.PushContext(Input::InputContextId{"editor.native_dialog.scene_save"},
                                                                     Input::InputContextKind::NativeDialog);
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
                    LOG_ERROR("editor.scene_document", "Scene destination '%s' could not be normalized: %s", destination.string().c_str(),
                              canonicalError.message().c_str());
                    return true;
                }

                EditorWorkspaceViewCommandData command;
                command.command = copyOnly ? EditorWorkspaceViewCommand::SaveSceneCopyAs : EditorWorkspaceViewCommand::SaveSceneAs;
                command.stringPayload = normalized.string();
                controller_->ProcessCommand(command);
                PublishViewportSceneIfChanged();
                return true;
            }

            bool workspacePersistenceAdmitted_{};

            void RestoreExtensionWorkspace() {
                auto *const activities = services_.TryGet<Extensions::EditorActivityHost>();
                if (!activities || !controller_)
                    return;
                const auto path = std::filesystem::path{controller_->ViewModel().projectRoot} / ".horo" / "editor_workspace.json";
                Extensions::EditorSurfaceWorkspaceState state{.schemaVersion = Extensions::EditorSurfaceRegistry::WorkspaceSchemaVersion};
                if (std::error_code existsError; std::filesystem::exists(path, existsError)) {
                    std::string error;
                    const auto layout = WorkspaceLayoutPersistence::Load(path, &error);
                    if (!layout) {
                        LOG_WARN("editor.workspace", "Cannot restore workspace state: %s", error.c_str());
                        return;
                    }
                    for (const auto &surface : layout->surfaces) {
                        std::optional<Extensions::EditorActivityPlacement> placement;
                        if (surface.activityPlacement)
                            placement = Extensions::EditorActivityPlacement{static_cast<Extensions::EditorActivitySide>(
                                                                                surface.activityPlacement->side),
                                                                            surface.activityPlacement->group,
                                                                            static_cast<std::int32_t>(surface.activityPlacement->order)};
                        state.surfaces.push_back({.surfaceId = surface.id,
                                                  .provider = {surface.extensionId, surface.moduleId},
                                                  .open = surface.open,
                                                  .focused = surface.focused,
                                                  .opaqueState = surface.state,
                                                  .activityVisible = surface.visible,
                                                  .activityPlacement = placement});
                    }
                } else if (existsError)
                    return;
                if (const auto restored = activities->Registry().Restore(state); restored.HasError()) {
                    LOG_WARN("editor.workspace", "Cannot admit workspace surface state: %s", restored.ErrorValue().message.c_str());
                    return;
                }
                workspacePersistenceAdmitted_ = true;
                activities->Update();
                while (const auto destination = view_.UpdateExtensionActivities())
                    controller_->ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                                 .targetIndex = static_cast<int>(*destination),
                                                 .stringPayload = std::string{}});
            }

            void SaveExtensionWorkspace() {
                auto *const activities = services_.TryGet<Extensions::EditorActivityHost>();
                if (!activities || !controller_ || !workspacePersistenceAdmitted_)
                    return;
                auto layout = controller_->ViewModel().workspacePanelHost.Layout();
                layout.surfaces.clear();
                for (const auto &surface : activities->Registry().Save().surfaces) {
                    std::optional<WorkspaceActivityPlacement> placement;
                    if (surface.activityPlacement)
                        placement = WorkspaceActivityPlacement{static_cast<WorkspaceActivitySide>(surface.activityPlacement->side),
                                                               surface.activityPlacement->group,
                                                               static_cast<std::uint32_t>(surface.activityPlacement->order)};
                    layout.surfaces.emplace_back(surface.surfaceId, surface.provider.extensionId, surface.provider.moduleId, surface.open,
                                                 surface.focused, surface.activityVisible, surface.opaqueState, placement);
                }
                std::string error;
                AtomicFileReplacementReceipt receipt;
                if (const auto path = std::filesystem::path{controller_->ViewModel().projectRoot} / ".horo" / "editor_workspace.json";
                    !projectServices_.durableFiles ||
                    !WorkspaceLayoutPersistence::Save(*projectServices_.durableFiles, receipt, path, layout, &error)) {
                    LOG_WARN("editor.workspace", "Cannot confirm workspace state publication: %s", error.c_str());
                    return;
                }
                workspacePersistenceAdmitted_ = false;
            }

            /** @brief Opens transient presentation over the exact root-owned project capability; unavailable preparation is recoverable. */
            void OpenPackagesModal() {
                if (!controller_ || !packages_.acquire)
                    return;
                const auto acquire = packages_.acquire;
                const std::filesystem::path root{controller_->ViewModel().projectRoot};
                const auto opened = modalHost_.OpenRoot(std::make_unique<PackageModal>(context_, [acquire, root] {
                    return acquire(root);
                }));
                if (opened.HasError())
                    LOG_WARN("editor.packages", "Package modal admission failed: %s", opened.ErrorValue().code.Value().c_str());
            }

            void OpenPlayTopologyProfilesModal() {
                if (!controller_ || !projectServices_.durableFiles)
                    return;
                const auto &model = controller_->ViewModel();
                if (model.playState != EditorPlayState::Idle && model.playState != EditorPlayState::Failed)
                    return;
                const std::filesystem::path root{model.projectRoot};
                auto store = std::make_unique<Application::PlayTopologyStore>(*projectServices_.durableFiles,
                                                                              root / "config" / "play_topologies.json",
                                                                              root / ".horo" / "local" / "play_topology_overrides.json");
                const auto opened = modalHost_.OpenRoot(std::make_unique<PlayTopologyModal>(context_, std::move(store)));
                if (opened.HasError())
                    LOG_WARN("editor.play_topology", "Profile modal admission failed: %s", opened.ErrorValue().message.c_str());
            }

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
                previousInputProfile_ = input_.router.Profile();
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

                const Result<void> applied = input_.router.SetProfile(std::move(merged));
                if (applied.HasError()) {
                    LOG_ERROR("editor.input", "Keeping last valid input profile for project '%s': %s", projectRoot.string().c_str(),
                              applied.ErrorValue().message.c_str());
                }
            }

            void RouteInputAction(EditorWorkspaceViewCommandData &command) {
                const auto pressed = [this](const char *id) {
                    return input_.router.ReadAction(input_.context, Input::ActionId{id}).pressed;
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
                    contributions_.statusItems
                        .Update("horo.status.document",
                                EditorStatusItemContent{.iconResourceId = "horo.status.document",
                                                        .label = context_.localization.Get("editor", viewModel.isDirty
                                                                                                         ? "status.document.unsaved"
                                                                                                         : "status.document.saved"),
                                                        .tone = viewModel.isDirty ? EditorStatusItemTone::Warning
                                                                                  : EditorStatusItemTone::Success,
                                                        .available = true}));
                static_cast<void>(
                    contributions_.statusItems
                        .Update("horo.status.selection",
                                EditorStatusItemContent{.value = context_.localization.Get("editor", viewModel.primarySelection.has_value()
                                                                                                         ? "status.selection.one"
                                                                                                         : "status.selection.none"),
                                                        .available = true}));
            }

            PackageWorkspaceAccess packages_;
            GuiScreenHost &host_;
            const EditorServiceRegistry &services_;
            EditorModalHost &modalHost_;
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
            std::unique_ptr<EditorWorkspaceController> controller_;
            mutable WorkspaceLeaveCapture leaveCapture_;
            std::unique_ptr<EditorSnackbarHost> snackbarHost_;
            std::optional<Input::InputBindingProfile> previousInputProfile_;
        };
    }  // namespace

    void RegisterEditorWorkspaceScreen(ScreenRegistry &registry, PackageWorkspaceAccess packages) {
        registry.Register(GuiRouteKind::EditorWorkspace,
                          [packages = std::move(packages)](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<EditorWorkspaceScreen>(services, packages);
        });
    }
}  // namespace Horo::Editor
