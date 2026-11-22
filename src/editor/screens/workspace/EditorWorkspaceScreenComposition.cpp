#include "EditorWorkspaceCommandAccess.h"
#include "EditorWorkspaceScreen.h"
#include "EnvironmentQueryPreviewNavigation.h"
#include "EnvironmentQueryPreviewPhysics.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/NavigationViewportSnapshot.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Editor/WorkspaceReadSources.h"
#include "Horo/Editor/XRSimulatorModel.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PCGPreviewWorkspace.h"
#include "ScriptModuleHostServices.h"
#include "editor/input/EditorInputActions.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "editor/source_editor/SourceReviewApplication.h"
#include "editor/source_editor/SourceReviewOverlay.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace Horo::Editor::WorkspaceScreenDetail {
    namespace {
        /** @brief Borrows the active Decision Graph projection only for the current owner-lane call. */
        const DecisionGraphWorkspaceDocument *ActiveDecisionDocument(const EditorWorkspaceViewModel &model) {
            const auto active = model.workspacePanelHost.ActiveDocument();
            const auto found = std::ranges::find_if(model.decisionGraphDocuments, [active](const auto &document) {
                return document.identity.instance == active;
            });
            return found == model.decisionGraphDocuments.end() ? nullptr : &*found;
        }

        /** @brief Copies complete Problems messages only, preserving order and recording omissions. */
        void CopyDecisionProblems(AgentEditorDiagnosticsSnapshot &snapshot,
                                  std::span<const AI::DecisionAssetValidationDiagnostic> diagnostics) {
            auto remaining = AgentEditorDiagnosticsSnapshot::MaximumMessageBytes;
            for (const auto &finding : diagnostics) {
                if (snapshot.messages.size() == AgentEditorDiagnosticsSnapshot::MaximumMessages) {
                    snapshot.truncated = true;
                    break;
                }
                const auto &code = finding.code.Value();
                constexpr std::size_t Separator = 2;
                if (remaining < Separator || code.size() > remaining - Separator ||
                    finding.message.size() > remaining - Separator - code.size()) {
                    snapshot.truncated = true;
                    continue;
                }
                snapshot.messages.push_back(code + ": " + finding.message);
                remaining -= snapshot.messages.back().size();
            }
        }

        /** @brief Composes the explicit project loader and retains its cooperative lease through cache publication. */
        PrefabHierarchySourceCapture PrefabCapture(const std::filesystem::path &root, const ProjectMutationCoordinator *mutations,
                                                   Prefab::PrefabMetricHandles metrics,
                                                   std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas) {
            return
                [root, mutations, metrics = std::move(metrics),
                 schemas = std::move(schemas)](const Assets::AssetRegistrySnapshot &registry, const std::span<const Assets::AssetId> roots,
                                               const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation) {
                if (mutations == nullptr)
                    return Result<PreparedPrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                auto lease = mutations->TryAcquire({root, ProjectMutationOwner::Asset, "prefab.hierarchy.capture"});
                if (lease.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(lease.ErrorValue());
                auto project = Application::LoadProjectSourceDocument(root);
                if (project.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(project.ErrorValue());
                auto sources = CapturePrefabHierarchySources(root, registry, roots, limits, project.Value().metadata.horoVersion.value,
                                                             cancellation, metrics, schemas);
                if (sources.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(sources.ErrorValue());
                return Result<PreparedPrefabHierarchySources>::Success({std::move(sources).Value(), std::move(lease).Value()});
            };
        }

        /** @brief Copies only explicitly registered override policy into the asynchronous source capture lease. */
        std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> OverrideSchemas(const EditorServiceRegistry &services) {
            const auto *schemas = services.TryGetConst<Prefab::PrefabValueSchemaSnapshot>();
            return schemas ? std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(*schemas) : nullptr;
        }

        /** @brief Owns the capability catalog and disposable preview inputs admitted together by the workspace host. */
        struct WorkspacePCGComposition final {
            PCGEditorCatalog catalog;
            PCGPreviewInputs previewInputs;
        };

        /** @brief Prepares PCG capabilities and sandbox inputs before publishing a workspace controller. */
        Result<WorkspacePCGComposition> CreatePCGWorkspaceComposition() {
            const std::array previewGrants{PCG::PCGCapability::Validation, PCG::PCGCapability::EditorPreview,
                                           PCG::PCGCapability::OfflineBake, PCG::PCGCapability::RuntimeEvaluation,
                                           PCG::PCGCapability::HybridEvaluation};
            auto pcgComposition = CreatePCGEditorCatalog(PCG::PCGCapabilitySet::Create(previewGrants).Value());
            if (pcgComposition.HasError())
                return Result<WorkspacePCGComposition>::Failure(pcgComposition.ErrorValue());
            auto pcgCatalog = std::move(pcgComposition).Value();
            auto pcgPreview = CreatePCGPreviewSandbox();
            if (pcgPreview.HasError())
                return Result<WorkspacePCGComposition>::Failure(pcgPreview.ErrorValue());
            return Result<WorkspacePCGComposition>::Success({std::move(pcgCatalog), std::move(pcgPreview).Value()});
        }

        /** @brief Compose one explicit resource lease without replacing the prior controller capability. */
        Result<std::unique_ptr<UiInspectorResources>> PrepareUiInspectorResources(const EditorServiceRegistry &services,
                                                                                  const std::filesystem::path &root,
                                                                                  const Assets::AssetId owner) {
            if (const auto *factory = services.TryGet<UiInspectorResourcesFactory>())
                return (*factory)(root, owner);
            return Result<std::unique_ptr<UiInspectorResources>>::Success(nullptr);
        }

        /** @brief Capture gameplay borrows and the selected play factory at the host composition boundary. */
        EditorWorkspaceGameplayServices BindGameplayServices(const EditorServiceRegistry &services, const GuiScreenHost &host,
                                                             EditorPlaySceneFactory sceneFactory) {
            const auto *environment = services.TryGet<Application::GameplayBuildEnvironment>();
            const auto *testEnvironment = services.TryGetConst<Application::GameplayTestEnvironment>();
            auto *saveRetirement = services.TryGet<EditorPlaySaveRetirement>();
            auto *moduleScripts = services.TryGet<Extensions::ScriptModuleHostServices>();
            return {.builds = services.TryGet<Application::GameplayBuildService>(),
                    .environment = environment ? *environment : Application::GameplayBuildEnvironment{},
                    .tests = services.TryGet<Application::GameplayTestService>(),
                    .testEnvironment = testEnvironment ? *testEnvironment : Application::GameplayTestEnvironment{},
                    .retirements = host.GameplayRetirements(),
                    .saveRetirement = saveRetirement ? saveRetirement->shared_from_this() : nullptr,
                    .moduleScripts = moduleScripts ? moduleScripts->shared_from_this() : nullptr,
                    .sceneFactory = std::move(sceneFactory)};
        }

    }  // namespace

    /** @copydoc EditorWorkspaceScreen::AttachWorkspacePanels */
    void EditorWorkspaceScreen::AttachWorkspacePanels(std::string_view projectIdentity) {
        const auto activeSource = [this](SourceDocumentSnapshot &output) {
            const auto &model = controller_->ViewModel();
            const auto active = model.workspacePanelHost.ActiveDocument();
            if (!active)
                return false;
            const auto found = std::ranges::find_if(model.sourceDocuments, [active](const SourceDocumentSnapshot &source) {
                return source.Revision() != 0 && source.Identity().instance == *active;
            });
            if (found == model.sourceDocuments.end())
                return false;
            output = *found;
            return true;
        };
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
            .extensionInspectors = services_.TryGet<Extensions::EditorInspectorHost>(),
            .navigationOverlays = services_.TryGetConst<NavigationViewportAccess>(),
            .extensionViewport = services_.TryGet<Extensions::EditorViewportHost>(),
            .extensionTheme = services_.TryGetConst<Extensions::EditorThemeFrame>(),
            .xrSimulatorArtifacts = simulatorArtifacts_,
            .xrSimulator = panelServices_.xrSimulator,
            .platformDiagnostics = panelServices_.platformDiagnostics,
            .sceneObjectCount = [this]() -> std::optional<std::size_t> {
            const auto &model = controller_->ViewModel();
            if (model.projectRoot.empty() || model.documentRevision.value == 0 || model.worldDocument)
                return std::nullopt;
            return model.objects.size();
        },
            .projectIdentity = projectIdentity,
            .projectFileRoot = &controller_->ProjectRoot(),
            .activeSourceSnapshot = activeSource,
            .activeSourceRange =
                [this, activeSource](SourceDocumentSnapshot &output, SourceContextRange &range) {
            SourceDocumentSnapshot source;
            if (!activeSource(source))
                return false;
            const auto selected = view_.ReadSourceSelection(source);
            if (!selected)
                return false;
            output = std::move(source);
            range = *selected;
            return true;
        },
            .openSourceSnapshots = [this]() -> std::span<const SourceDocumentSnapshot> {
            return controller_->ViewModel().sourceDocuments;
        },
            .captureSceneSelection =
                [this] {
            return controller_->CaptureAgentSceneSelection();
        },
            .agentProjectFiles = agentProjectFiles_,
            .agentProjectReadIdentity = agentProjectReadIdentity_,
            .captureEditorDiagnostics =
                [this] {
            return CaptureEditorDiagnostics();
        },
            .requestSourceReview =
                [](const Proposals::ChangeSet &proposal, const Proposals::WorkspaceEditPlan &plan, const std::size_t operation,
                   EditorWorkspaceViewCommandData &command) {
            if (command.command != EditorWorkspaceViewCommand::None)
                return false;
            auto overlay = SourceReviewOverlay::Capture(proposal, plan, operation);
            if (overlay.HasError())
                return false;
            command.sourceReview = std::make_shared<const SourceReviewOverlay>(std::move(overlay).Value());
            command.command = EditorWorkspaceViewCommand::ReviewSourceProposal;
            return true;
        },
            .requestSourceReviewApply =
                [project = std::string{projectIdentity}](const Proposals::ReviewDecisions &decisions,
                                                         const Proposals::ProjectPolicy &policy,
                                                         const std::shared_ptr<const Proposals::WorkspaceEditPlan> &plan,
                                                         const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) {
            return QueueSourceReviewApplication(project, decisions, policy, plan, lifetime, command);
        },
            .requestSourceReviewUndo =
                [this](const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) {
            return controller_->SourceReviews().QueueUndo(lifetime, command);
        },
            .sourceReviewTransaction =
                [this] {
            return controller_->SourceReviews().Status();
        },
            .requestMcpAudit =
                [this, lifetime = std::weak_ptr<const void>{workspaceControllerLifetime_}](const WorkspaceMcpAuditReference &reference,
                                                                                           EditorWorkspaceViewCommandData &command) {
            if (lifetime.expired() || command.command != EditorWorkspaceViewCommand::None)
                return;
            for (const auto &panel : contributions_.panels.GetAllPanels()) {
                if (auto *dock = dynamic_cast<GlobalDockPanel *>(panel.get()); dock && dock->FocusMcpAudit(reference)) {
                    controller_->StagePanelActivation(dock->GetId(), dock->GetDefaultDockArea(), command);
                    return;
                }
            }
        },
            .captureAgentViewport =
                [this](const AgentViewportComponents components) {
            return controller_->CaptureAgentViewport(components);
        },
        };
        if (std::ranges::any_of(contributions_.panels.GetAllPanels(), [](const auto &panel) {
            return panel->GetId() == LocalAgentWorkspacePanelId;
        })) {
            const auto viewportOffer = std::make_shared<std::optional<AgentSceneSelectionSnapshot>>();
            panelContext.takeAgentViewportReview = [viewportOffer] {
                return std::exchange(*viewportOffer, std::nullopt);
            };
            panelContext.offerAgentViewportReview =
                [this, viewportOffer,
                 lifetime = std::weak_ptr<const void>{workspaceControllerLifetime_}](EditorWorkspaceViewCommandData &command) {
                if (lifetime.expired() || command.command != EditorWorkspaceViewCommand::None)
                    return;
                if (auto snapshot = controller_->CaptureAgentViewport(AgentViewportComponents::None)) {
                    *viewportOffer = std::move(snapshot);
                    controller_->StagePanelActivation(LocalAgentWorkspacePanelId, WorkspaceDockArea::Right, command);
                }
            };
        }
        contributions_.panels.AttachAll(panelContext);
    }

    /** @copydoc EditorWorkspaceScreen::AttachWorkspaceCommands */
    void EditorWorkspaceScreen::AttachWorkspaceCommands(std::string projectIdentity) {
        auto *commands = services_.TryGet<IEditorWorkspaceCommandHost>();
        if (!commands || projectIdentity.empty())
            return;
        auto *const workspace = controller_.get();
        const std::weak_ptr<const void> lifetime = workspaceControllerLifetime_;
        EditorWorkspaceCommandAccess access{
            .projectIdentity = projectIdentity,
            .bind =
                [workspace, lifetime, project = std::move(projectIdentity)](const std::uint64_t trustRevision) {
            if (lifetime.expired())
                return EditorWorkspaceCommandEndpoints{};
            return EditorWorkspaceCommandEndpoints{
                workspace->BindSourcePresentation(project, trustRevision),
                workspace->BindGameplayBuildCommands(project, trustRevision),
                workspace->BindGameplayTestCommands(project, trustRevision),
            };
        },
            .revoke =
                [workspace, lifetime] {
            if (!lifetime.expired())
                workspace->RevokeWorkspaceCommands();
        },
        };
        if (const auto attached = commands->Attach(std::move(access)); attached.HasError()) {
            LOG_WARN("editor.mcp", "Unable to attach workspace commands: %s", attached.ErrorValue().message.c_str());
            return;
        }
        workspaceCommands_ = commands;
    }

    /** @copydoc EditorWorkspaceScreen::DetachWorkspaceCommands */
    void EditorWorkspaceScreen::DetachWorkspaceCommands() {
        auto *commands = std::exchange(workspaceCommands_, nullptr);
        if (!commands)
            return;
        if (const auto detached = commands->Detach(); detached.HasError())
            LOG_WARN("editor.mcp", "Workspace command retirement was incomplete: %s", detached.ErrorValue().message.c_str());
    }

    /** @copydoc EditorWorkspaceScreen::PumpWorkspaceCommands */
    void EditorWorkspaceScreen::PumpWorkspaceCommands() {
        if (!workspaceCommands_)
            return;
        if (const auto pumped = workspaceCommands_->PumpWorkspaceCommands(); pumped.HasError()) {
            LOG_WARN("editor.mcp", "Cannot pump workspace commands: %s", pumped.ErrorValue().message.c_str());
            DetachWorkspaceCommands();
        }
    }

    /** @copydoc EditorWorkspaceScreen::CaptureEditorDiagnostics */
    std::optional<AgentEditorDiagnosticsSnapshot> EditorWorkspaceScreen::CaptureEditorDiagnostics() {
        ReconcileEditorDiagnostics();
        const auto *document = ActiveDecisionDocument(controller_->ViewModel());
        if (!document || !document->validation || document->validation->Empty())
            return std::nullopt;
        AgentEditorDiagnosticsSnapshot snapshot{.document = document->identity.instance.Value(),
                                                .observation = ++editorDiagnosticsSequence_};
        CopyDecisionProblems(snapshot, document->validation->Diagnostics());
        if (snapshot.messages.empty())
            return std::nullopt;
        if (!editorDiagnosticsReview_)
            editorDiagnosticsReview_.emplace(
                EditorDiagnosticsReview{document->identity.instance, document->revision, document->validation});
        snapshot.lifetime = editorDiagnosticsReview_->lifetime.Token();
        return snapshot;
    }

    /** @copydoc EditorWorkspaceScreen::ReconcileEditorDiagnostics */
    void EditorWorkspaceScreen::ReconcileEditorDiagnostics() {
        if (!editorDiagnosticsReview_)
            return;
        const auto *document = ActiveDecisionDocument(controller_->ViewModel());
        if (!document || document->identity.instance != editorDiagnosticsReview_->document ||
            document->revision != editorDiagnosticsReview_->revision || document->validation != editorDiagnosticsReview_->validation)
            RevokeEditorDiagnostics();
    }

    /** @copydoc EditorWorkspaceScreen::RevokeEditorDiagnostics */
    void EditorWorkspaceScreen::RevokeEditorDiagnostics() noexcept {
        if (editorDiagnosticsReview_) {
            editorDiagnosticsReview_->lifetime.RequestCancellation();
            editorDiagnosticsReview_.reset();
        }
    }

    /** @copydoc EditorWorkspaceScreen::BindPlayFactory */
    EditorPlaySceneFactory EditorWorkspaceScreen::BindPlayFactory(const std::filesystem::path &projectRoot) const {
        EditorPlaySceneFactory playSceneFactory;
        if (const auto *projectFactory = services_.TryGet<EditorProjectPlaySceneFactory>()) {
            // Copy the application closure and exact project path into the workspace. No
            // reference to this screen or its activation stack survives asynchronous Start.
            playSceneFactory = [factory = *projectFactory, root = projectRoot](const Runtime::SceneRuntimeId identity,
                                                                               const Runtime::RuntimeSceneDefinition &definition) {
                return factory(root, identity, definition);
            };
        }
        return playSceneFactory;
    }

    /** @copydoc EditorWorkspaceScreen::BindPrefabStageFactory */
    EditorPlaySceneFactory EditorWorkspaceScreen::BindPrefabStageFactory(const std::filesystem::path &projectRoot) const {
        const auto *factory = services_.TryGet<EditorProjectPrefabStageFactory>();
        if (!factory || !factory->create)
            return {};
        return [create = factory->create, root = projectRoot](Runtime::SceneRuntimeId identity,
                                                              const Runtime::RuntimeSceneDefinition &definition) {
            return create(root, identity, definition);
        };
    }

    /** @copydoc EditorWorkspaceScreen::RevokeSimulatorArtifacts */
    void EditorWorkspaceScreen::RevokeSimulatorArtifacts() noexcept {
        if (simulatorArtifacts_)
            simulatorArtifacts_->Revoke();
        simulatorArtifacts_.reset();
    }

    /** @copydoc EditorWorkspaceScreen::InitializeController */
    Result<void> EditorWorkspaceScreen::InitializeController(const std::filesystem::path &projectRoot) {
        const Assets::AssetRegistrySnapshot assetSnapshot =
            projectServices_.assetRegistry ? projectServices_.assetRegistry->Snapshot() : Assets::AssetRegistrySnapshot{};
        auto capturedSources = CaptureWorkspaceReadSources(services_.TryGetConst<WorkspaceReadSourceFactory>(), projectRoot);
        if (capturedSources.HasError())
            return Result<void>::Failure(capturedSources.ErrorValue());
        auto sources = std::move(capturedSources).Value();
        auto simulatorArtifacts = sources.simulatorArtifacts;
        auto agentProjectFiles = sources.world;
        auto agentProjectReadIdentity = sources.owner.ToString();
        auto resources = PrepareUiInspectorResources(services_, projectRoot, sources.owner);
        if (resources.HasError())
            return Result<void>::Failure(resources.ErrorValue());
        auto uiInspectorResources = std::move(resources).Value();
        auto pcgComposition = CreatePCGWorkspaceComposition();
        if (pcgComposition.HasError())
            return Result<void>::Failure(std::move(pcgComposition).ErrorValue());
        auto pcg = std::move(pcgComposition).Value();
        view_.ResetSourceSession();
        controller_ = std::make_unique<EditorWorkspaceController>(projectRoot, runtimeScene_, assetSnapshot,
                                                                  BindControllerDependencies(projectRoot, uiInspectorResources.get(),
                                                                                             std::move(sources), std::move(pcg.catalog),
                                                                                             std::move(pcg.previewInputs)));
        if (controller_->InitializationError().has_value()) {
            const Error error = *controller_->InitializationError();
            controller_.reset();
            return Result<void>::Failure(error);
        }
        if (auto *runner = services_.TryGet<IExternalProcessRunner>())
            controller_->BindSourceLanguage(*runner, Language::CaptureClangdHostSettings());
        controller_->SelectMountedWorldPlay(services_.TryGetConst<EditorMountedWorldPlaySelected>() != nullptr);
        RevokeSimulatorArtifacts();
        simulatorArtifacts_ = std::move(simulatorArtifacts);
        agentProjectFiles_ = std::move(agentProjectFiles);
        agentProjectReadIdentity_ = std::move(agentProjectReadIdentity);
        uiInspectorResources_ = std::move(uiInspectorResources);
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::BindControllerDependencies */
    EditorWorkspaceDependencies EditorWorkspaceScreen::BindControllerDependencies(const std::filesystem::path &projectRoot,
                                                                                  UiInspectorResources *uiInspectorResources,
                                                                                  WorkspaceReadSources &&sources,
                                                                                  PCGEditorCatalog &&catalog,
                                                                                  PCGPreviewInputs &&previewInputs) const {
        const auto *bakeDependencies = services_.TryGetConst<Application::WorldBakeDependencySource>();
        const auto *latticeFactory = services_.TryGetConst<AI::LatticeQueryAuthoringFactory>();
        const auto *contextAccess = services_.TryGetConst<AI::QueryContextProviderAccess>();
        return EditorWorkspaceDependencies{.assets = {.mutableAssetRegistry = projectServices_.assetRegistry,
                                                      .source = std::move(sources.registry),
                                                      .importerCatalog = projectServices_.importerCatalog},
                                           .world = {.sourceFiles = std::move(sources.world),
                                                     .documentSessionOwner = sources.owner,
                                                     .streamingPreviewPolicy = services_.TryGetConst<WorldStreamingPreviewRuntimePolicy>(),
                                                     .streamingPreview = services_.TryGet<WorldStreamingPreviewHost>()},
                                           .worldBake = {.operations = services_.TryGet<OperationStore>(),
                                                         .output = services_.TryGet<BuildOutputStore>(),
                                                         .dependencies =
                                                             bakeDependencies ? std::optional{*bakeDependencies} : std::nullopt},
                                           .uiInspectorResources = uiInspectorResources,
                                           .collision = {.inventory = projectServices_.collisionInventory,
                                                         .activation = projectServices_.collisionActivation,
                                                         .previewReceipt = projectServices_.previewCollisionReceipt},
                                           .mutations = projectServices_.mutations,
                                           .durableFiles = projectServices_.durableFiles,
                                           .jobs = &services_.Get<JobSystem>(),
                                           .sourceNavigation = {.diagnosticOutput = services_.TryGet<BuildOutputStore>()},
                                           .gameplay = BindGameplayServices(services_, host_, BindPlayFactory(projectRoot)),
                                           .localization = &context_.localization,
                                           .engineEvents = &context_.engineEvents,
                                           .networkDebugger = services_.TryGet<Application::NetworkDebuggerService>(),
                                           .inspectorHost = services_.TryGet<Extensions::EditorInspectorHost>(),
                                           .audioPreview = services_.TryGet<IAudioComponentPreview>(),
                                           .prefab = {.sources = PrefabCapture(projectRoot, projectServices_.mutations,
                                                                               services_.TryGetConst<Prefab::PrefabMetricHandles>()
                                                                                   ? *services_.TryGetConst<Prefab::PrefabMetricHandles>()
                                                                                   : Prefab::PrefabMetricHandles{},
                                                                               OverrideSchemas(services_)),
                                                      .stageFactory = BindPrefabStageFactory(projectRoot)},
                                           .procedural = {.catalog = std::move(catalog), .previewInputs = std::move(previewInputs)},
                                           .terrainCook = services_.TryGetConst<Application::TerrainAuthoringCookFactory>(),
                                           .terrainAssets = services_.TryGetConst<Application::TerrainAuthoringAssetFactory>(),
                                           .latticeQueryFactory = latticeFactory ? *latticeFactory : nullptr,
                                           .querySchema = services_.TryGetConst<AI::QuerySchemaRegistry>(),
                                           .queryContextAccess = contextAccess ? *contextAccess : AI::QueryContextProviderAccess{},
                                           .queryPreviewPhysics = services_.TryGetConst<EnvironmentQueryPreviewPhysics>(),
                                           .queryPreviewNavigation = services_.TryGetConst<EnvironmentQueryPreviewNavigation>()};
    }

    /** @copydoc EditorWorkspaceScreen::RestoreExtensionWorkspace */
    void EditorWorkspaceScreen::RestoreExtensionWorkspace() {
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
                    placement =
                        Extensions::EditorActivityPlacement{static_cast<Extensions::EditorActivitySide>(surface.activityPlacement->side),
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
        contributions_.workspacePersistenceAdmitted = true;
        activities->Update();
        while (const auto destination = view_.UpdateExtensionActivities())
            controller_->ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                         .targetIndex = static_cast<int>(*destination),
                                         .stringPayload = std::string{}});
    }

    /** @copydoc EditorWorkspaceScreen::SaveExtensionWorkspace */
    void EditorWorkspaceScreen::SaveExtensionWorkspace() {
        auto *const activities = services_.TryGet<Extensions::EditorActivityHost>();
        if (!activities || !controller_ || !contributions_.workspacePersistenceAdmitted)
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
        contributions_.workspacePersistenceAdmitted = false;
    }

    /** @copydoc EditorWorkspaceScreen::LoadProjectInputProfile */
    void EditorWorkspaceScreen::LoadProjectInputProfile(const std::filesystem::path &projectRoot) {
        input_.previousProfile = input_.router.Profile();
        input_.profilePaths.reset();
        const auto metadata = Application::LoadProjectMetadata(projectRoot);
        if (metadata.HasError()) {
            LOG_ERROR("editor.input", "Keeping last valid input profile; project metadata is unavailable: %s",
                      metadata.ErrorValue().message.c_str());
            return;
        }
        const auto paths = Input::ResolveBindingProfilePaths(projectRoot, ResolveEditorSettingsHomeDirectory(), metadata.Value().projectId);
        if (paths.HasError()) {
            LOG_ERROR("editor.input", "Keeping last valid input profile; project profile path is invalid: %s",
                      paths.ErrorValue().message.c_str());
            return;
        }
        const auto &editorProfile = paths.Value().editorUser;
        input_.profilePaths = paths.Value();
        std::error_code error;
        const bool exists = std::filesystem::exists(editorProfile, error);
        if (error) {
            LOG_ERROR("editor.input", "Keeping last valid input profile; editor profile is inaccessible: %s", error.message().c_str());
            return;
        }
        if (!exists) {
            const auto migrated = Input::SaveBindingProfileAtomically(editorProfile, settings_.Snapshot().settings.keymap.profile);
            if (migrated.HasError()) {
                LOG_ERROR("editor.input", "Keeping last valid input profile; user profile migration failed: %s",
                          migrated.ErrorValue().message.c_str());
                return;
            }
        }
        const std::array ordered{paths.Value().projectDefaults, paths.Value().editorUser, paths.Value().projectUser};
        auto loaded = Input::LoadBindingProfileLayers(ordered);
        if (loaded.HasError()) {
            LOG_ERROR("editor.input", "Keeping last valid input profile; project profile layering failed: %s",
                      loaded.ErrorValue().message.c_str());
            return;
        }
        auto merged = std::move(loaded).Value();
        const Result<void> applied = ActivateEditorInputProfile(input_.router, std::move(merged), settings_.ContributedInputActions());
        if (applied.HasError()) {
            LOG_ERROR("editor.input", "Keeping last valid input profile for project '%s': %s", projectRoot.string().c_str(),
                      applied.ErrorValue().message.c_str());
        }
    }

}  // namespace Horo::Editor::WorkspaceScreenDetail
