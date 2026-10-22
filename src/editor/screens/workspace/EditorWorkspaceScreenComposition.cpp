#include "EditorWorkspaceScreen.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/NavigationViewportSnapshot.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Editor/WorkspaceReadSources.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PCGPreviewWorkspace.h"

#include <array>
#include <span>
#include <utility>

namespace Horo::Editor::WorkspaceScreenDetail {
    namespace {
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
        EditorWorkspaceGameplayServices BindGameplayServices(const EditorServiceRegistry &services, GuiScreenHost &host,
                                                             EditorPlaySceneFactory sceneFactory) {
            const auto *environment = services.TryGet<Application::GameplayBuildEnvironment>();
            return {.builds = services.TryGet<Application::GameplayBuildService>(),
                    .environment = environment ? *environment : Application::GameplayBuildEnvironment{},
                    .retirements = host.GameplayRetirements(),
                    .sceneFactory = std::move(sceneFactory)};
        }

    }  // namespace

    /** @copydoc EditorWorkspaceScreen::AttachWorkspacePanels */
    void EditorWorkspaceScreen::AttachWorkspacePanels() {
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
        };
        contributions_.panels.AttachAll(panelContext);
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

    /** @copydoc EditorWorkspaceScreen::InitializeController */
    Result<void> EditorWorkspaceScreen::InitializeController(const std::filesystem::path &projectRoot) {
        const Assets::AssetRegistrySnapshot assetSnapshot =
            projectServices_.assetRegistry ? projectServices_.assetRegistry->Snapshot() : Assets::AssetRegistrySnapshot{};
        auto capturedSources = CaptureWorkspaceReadSources(services_.TryGetConst<WorkspaceReadSourceFactory>(), projectRoot);
        if (capturedSources.HasError())
            return Result<void>::Failure(capturedSources.ErrorValue());
        auto sources = std::move(capturedSources).Value();
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
        return EditorWorkspaceDependencies{.mutableAssetRegistry = projectServices_.assetRegistry,
                                           .uiInspectorResources = uiInspectorResources,
                                           .world = {.collision = {.inventory = projectServices_.collisionInventory,
                                                                   .activation = projectServices_.collisionActivation,
                                                                   .previewReceipt = projectServices_.previewCollisionReceipt},
                                                     .streamingPolicy = services_.TryGetConst<WorldStreamingPreviewRuntimePolicy>(),
                                                     .streamingPreview = services_.TryGet<WorldStreamingPreviewHost>(),
                                                     .files = std::move(sources.world)},
                                           .mutations = projectServices_.mutations,
                                           .durableFiles = projectServices_.durableFiles,
                                           .assetRegistrySource = std::move(sources.registry),
                                           .importerCatalog = projectServices_.importerCatalog,
                                           .jobs = &services_.Get<JobSystem>(),
                                           .worldBake = {.operations = services_.TryGet<OperationStore>(),
                                                         .output = services_.TryGet<BuildOutputStore>(),
                                                         .dependencies =
                                                             bakeDependencies ? std::optional{*bakeDependencies} : std::nullopt},
                                           .gameplay = BindGameplayServices(services_, host_, BindPlayFactory(projectRoot)),
                                           .localization = &context_.localization,
                                           .engineEvents = &context_.engineEvents,
                                           .networkDebugger = services_.TryGet<Application::NetworkDebuggerService>(),
                                           .documentSessionOwner = sources.owner,
                                           .inspectorHost = services_.TryGet<Extensions::EditorInspectorHost>(),
                                           .audioPreview = services_.TryGet<IAudioComponentPreview>(),
                                           .prefab = {.sources = PrefabCapture(projectRoot, projectServices_.mutations),
                                                      .stageFactory = BindPrefabStageFactory(projectRoot)},
                                           .procedural = {.catalog = std::move(catalog), .previewInputs = std::move(previewInputs)}};
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
        Input::InputBindingProfile merged{.profileId = "project-composed"};
        const auto mergeProfile = [&](const Input::InputBindingProfile &profile, const std::string &source) {
            Result<Input::InputBindingProfile> layered = Input::MergeBindingProfiles(merged, profile);
            if (layered.HasError()) {
                LOG_ERROR("editor.input", "Unable to layer input profile '%s': %s", source.c_str(), layered.ErrorValue().message.c_str());
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
        if (!mergeFile(projectRoot / ".horo" / "input.json") || !mergeProfile(*input_.previousProfile, "editor-global")) {
            return;
        }
        if (const Result<Application::ProjectMetadata> metadata = Application::LoadProjectMetadata(projectRoot);
            metadata.HasValue() &&
            !mergeFile(ResolveEditorSettingsHomeDirectory() / ".horo" / "input" / "projects" / (metadata.Value().projectId + ".json"))) {
            return;
        }

        const Result<void> applied = input_.router.SetProfile(std::move(merged));
        if (applied.HasError()) {
            LOG_ERROR("editor.input", "Keeping last valid input profile for project '%s': %s", projectRoot.string().c_str(),
                      applied.ErrorValue().message.c_str());
        }
    }

}  // namespace Horo::Editor::WorkspaceScreenDetail
