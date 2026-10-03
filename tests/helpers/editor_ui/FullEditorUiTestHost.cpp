#include "FullEditorUiTestHost.h"

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/RecentProject.h"
#include "Horo/Editor/RecentProjectInspectionService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/input/EditorInputActions.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/renderer/EditorViewportRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <imgui.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::Tests {
    namespace {
        class ScopedHome final {
        public:
            explicit ScopedHome(const std::filesystem::path &home) {
#if defined(_WIN32)
                if (const char *value = std::getenv("USERPROFILE"))
                    previous_ = value;
                _putenv_s("USERPROFILE", home.string().c_str());
#else
                if (const char *value = std::getenv("HOME"))
                    previous_ = value;
                setenv("HOME", home.string().c_str(), 1);
#endif
            }

            ~ScopedHome() {
#if defined(_WIN32)
                _putenv_s("USERPROFILE", previous_.c_str());
#else
                if (previous_.empty())
                    unsetenv("HOME");
                else
                    setenv("HOME", previous_.c_str(), 1);
#endif
            }

        private:
            std::string previous_;
        };

        [[nodiscard]] std::filesystem::path MakeIsolatedRoot() {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            return std::filesystem::temp_directory_path() / ("horo-full-editor-ui-" + std::to_string(nonce));
        }

        [[nodiscard]] Editor::RendererAvailabilitySnapshot MakeRendererAvailability(const IEditorUiTestSurface &surface) {
            const bool useMetal = surface.RendererName() == "metal";
            const std::string backendId = useMetal ? "metal" : "opengl";
            const std::string displayName = useMetal ? "Metal" : "OpenGL";
            return Editor::RendererAvailabilitySnapshot({Editor::RendererBackendAvailability{backendId,
                                                                                             displayName,
                                                                                             Editor::RendererAvailabilityState::Active,
                                                                                             {}}},
                                                        backendId);
        }

        void LoadLocalization(Editor::LocalizationService &localization, const std::string_view locale) {
            Editor::LocalizationError error;
            const std::filesystem::path root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
            if (!localization.LoadCatalogFile(root / "en-US.json", &error) || !localization.LoadCatalogFile(root / "tr-TR.json", &error) ||
                !localization.Prepare(Editor::LocaleTag{std::string{locale}}, &error) || !localization.ActivatePrepared(&error)) {
                throw std::runtime_error("Unable to load deterministic editor localization catalogs.");
            }
        }

        void WriteEmptySceneFixture(const std::filesystem::path &path) {
            std::ofstream scene(path, std::ios::binary);
            scene << R"({"schemaVersion":1,"objects":[]})";
            scene.close();
            if (!scene)
                throw std::runtime_error("Unable to write recent-project E2E scene.");
        }

        void CreateFixtureRoots(const std::filesystem::path &home, const std::filesystem::path &projectsRoot) {
            std::filesystem::create_directories(home);
            std::filesystem::create_directories(projectsRoot);
        }

        [[nodiscard]] std::filesystem::path SeedRecentProjectFixture(const std::filesystem::path &projectsRoot, const std::string &name,
                                                                     const IEditorUiTestSurface &surface) {
            const Application::EngineReleaseVersion release = Application::CurrentEngineReleaseVersion();
            const Application::ReleaseCompatibilityDecision *const decision =
                Application::BuiltInReleaseCompatibilityRegistry().Find(release);
            if (decision == nullptr)
                throw std::runtime_error("Current engine release is absent from the compatibility catalog.");

            const std::filesystem::path projectRoot = projectsRoot / name;
            std::filesystem::create_directories(projectRoot / ".horo");
            std::filesystem::create_directories(projectRoot / "assets/scenes");
            const nlohmann::json document{{"horoVersion", Application::FormatHoroVersion(release.value)},
                                          {"persistentContract", Application::FormatPersistentContractHash(decision->persistentContract)},
                                          {"projectId", "recent-project-e2e"},
                                          {"name", name},
                                          {"projectVersion", "0.1.0"},
                                          {"createdAt", "2026-07-22T00:00:00Z"},
                                          {"settings",
                                           {{"renderBackend", surface.RendererName() == "metal" ? "metal" : "opengl"},
                                            {"defaultScene", "assets/scenes/main.horo"}}}};
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << document.dump(2) << '\n';
            metadata.close();
            if (!metadata)
                throw std::runtime_error("Unable to write recent-project E2E metadata.");
            WriteEmptySceneFixture(projectRoot / "assets/scenes/main.horo");

            if (!Editor::SaveRecentProjectsToDisk(
                    {Editor::RecentProjectEntry{name, projectRoot.string(), "Just now", "empty", std::nullopt}})) {
                throw std::runtime_error("Unable to seed the recent-project E2E list.");
            }
            return projectRoot;
        }
    }  // namespace

    struct FullEditorUiTestHost::State {
        State(IEditorUiTestSurface &testSurface, std::string locale, std::optional<std::string> recentProjectName)
            : root(MakeIsolatedRoot()), home(root / "home"), projectsRoot(root / "projects"), scopedHome(home),
              jobs(JobSystemConfig{2, 256}), creation(jobs, engineEvents), localization(Editor::LocaleTag{"en-US"}),
              configuration(Editor::CreateEditorConfigurationService(Editor::DefaultEditorSettings())),
              settings(Editor::DefaultEditorSettings(), configuration, editorEvents, localization), modals(editorEvents, input),
              mutations(files), transactions(files, wallClock, mutations, jobs),
              gameplayBuilds(externalProcesses, jobs, files, &buildOutput, &operations),
              gameplayBuildEnvironment{.gameplaySdkPackage = HORO_GAMEPLAY_SDK_PACKAGE_DIR,
                                       .cxxCompiler = std::filesystem::path{HORO_GAMEPLAY_CXX_COMPILER}},
              preflight(transactions), recentInspection(jobs, preflight), rendererAvailability(MakeRendererAvailability(testSurface)),
              open(jobs, files, preflight, mutations, transactions, rendererAvailability), surface(testSurface),
              viewportRenderer(testSurface.ViewportRenderer()),
              fonts{ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault},
              theme{fonts}, settingsSnapshot(settings.Snapshot()), gui{engineEvents, editorEvents, localization, theme, settingsSnapshot} {
            CreateFixtureRoots(home, projectsRoot);
            LoadLocalization(localization, locale);
            if (recentProjectName.has_value())
                static_cast<void>(SeedRecentProjectFixture(projectsRoot, *recentProjectName, testSurface));
            static_cast<void>(input.SetActionMap(Editor::BuildEditorInputActions()));
            Editor::ScreenRegistry screens;
            Editor::RegisterWelcomeScreen(screens);
            Editor::RegisterProjectCreationScreen(screens);
            Editor::RegisterProjectLoadingScreen(screens);
            Editor::RegisterEditorWorkspaceScreen(screens);
            Editor::WorkspacePanelRegistry panels;
            Editor::RegisterDefaultWorkspacePanels(panels);
            screenHost = std::make_unique<Editor::GuiScreenHost>(gui, modals, settings, localization, engineEvents, creation, jobs, input,
                                                                 rendererAvailability, std::move(screens), std::move(panels));
            screenHost->Services().Register<Editor::IEditorViewportRenderer>(viewportRenderer);
            screenHost->Services().Register<Editor::EditorViewportSceneState>(viewportScene);
            screenHost->Services().Register<Runtime::RuntimeSceneService>(runtimeScene);
            screenHost->Services().Register<Assets::AssetRegistry>(assetRegistry);
            screenHost->Services().Register<Editor::ProjectMutationCoordinator>(mutations);
            screenHost->Services().Register<DurableFileSystem>(files);
            screenHost->Services().Register<Application::GameplayBuildService>(gameplayBuilds);
            screenHost->Services().Register<Application::GameplayBuildEnvironment>(gameplayBuildEnvironment);
            screenHost->Services().RegisterConst<IBuildOutputQuery>(buildOutput);
            screenHost->Services().Register<OperationStore>(operations);
            screenHost->Services().RegisterConst<IOperationQuery>(operations);
            screenHost->Services().Register<IOperationControl>(operations);
            screenHost->Services().Register<Editor::ProjectOpenService>(open);
            screenHost->Services().Register<Editor::RecentProjectInspectionService>(recentInspection);
            const Result<void> started = screenHost->Start({Editor::GuiRouteKind::Welcome, Editor::WelcomeRouteParameters{}});
            if (started.HasError())
                throw std::runtime_error(started.ErrorValue().message);
            const CancellationToken token = runtimeCancellation.Token();
            const Result<void> runtimeStarted = runtimeScene.Startup(token);
            if (runtimeStarted.HasError())
                throw std::runtime_error(runtimeStarted.ErrorValue().message);
        }

        ~State() {
            if (screenHost)
                screenHost->Shutdown();
            runtimeScene.Shutdown();
            gameplayBuilds.Shutdown();
            recentInspection.Shutdown();
            open.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
        std::filesystem::path home;
        std::filesystem::path projectsRoot;
        ScopedHome scopedHome;
        EngineDataBus engineEvents;
        Editor::EditorDataBus editorEvents;
        JobSystem jobs;
        Editor::ProjectCreationService creation;
        Editor::LocalizationService localization;
        ConfigurationService configuration;
        Editor::EditorSettingsService settings;
        Input::InputRouter input;
        Editor::EditorModalHost modals;
        NativeDurableFileSystem files;
        NativeExternalProcessRunner externalProcesses;
        BuildOutputStore buildOutput{2048U};
        OperationStore operations{64U, 200U};
        SystemWallClock wallClock;
        Editor::ProjectMutationCoordinator mutations;
        Editor::ProjectMigrationTransactionService transactions;
        Application::GameplayBuildService gameplayBuilds;
        Application::GameplayBuildEnvironment gameplayBuildEnvironment;
        Editor::ProjectOpenPreflightService preflight;
        Editor::RecentProjectInspectionService recentInspection;
        Editor::RendererAvailabilitySnapshot rendererAvailability;
        Editor::ProjectOpenService open;
        Assets::AssetRegistry assetRegistry;
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource runtimeCancellation;
        std::uint64_t runtimeFrameNumber{};
        std::uint64_t runtimeSimulationTick{};
        Editor::EditorViewportSceneState viewportScene;
        IEditorUiTestSurface &surface;
        Editor::IEditorViewportRenderer &viewportRenderer;
        Editor::Theme::Fonts fonts;
        Editor::ThemeContext theme;
        Editor::EditorSettingsSnapshot settingsSnapshot;
        Editor::EditorGuiContext gui;
        std::unique_ptr<Editor::GuiScreenHost> screenHost;
        std::vector<Editor::GuiRouteKind> drawnRoutes;
        std::optional<Editor::EditorMenuInvocation> pendingMenuInvocation;
    };

    FullEditorUiTestHost::FullEditorUiTestHost(IEditorUiTestSurface &surface, std::string locale,
                                               std::optional<std::string> recentProjectName)
        : state_(std::make_unique<State>(surface, std::move(locale), std::move(recentProjectName))) {}

    FullEditorUiTestHost::~FullEditorUiTestHost() = default;

    void FullEditorUiTestHost::DrawFrame(ImGuiTestContext *) {
        state_->engineEvents.DispatchQueued();
        if (state_->pendingMenuInvocation.has_value()) {
            state_->screenHost->DispatchMenuInvocation(*state_->pendingMenuInvocation);
            state_->pendingMenuInvocation.reset();
        }
        state_->settingsSnapshot = state_->settings.Snapshot();
        state_->modals.OnUpdate(1.0F / 60.0F);
        state_->screenHost->OnUpdate(1.0F / 60.0F);
        state_->drawnRoutes.push_back(state_->screenHost->ActiveRoute().kind);
        state_->screenHost->Draw();
        state_->modals.Draw();
        state_->surface.RenderViewport(state_->viewportScene.View());
        const CancellationToken token = state_->runtimeCancellation.Token();
        const Runtime::FrameContext context{++state_->runtimeFrameNumber, Duration::FromMilliseconds(16), 0.0, 0, {}, false, token};
        const Result<void> committed = state_->runtimeScene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context);
        if (committed.HasError())
            throw std::runtime_error(committed.ErrorValue().message);
    }

    void FullEditorUiTestHost::AdvanceFixedTicks(const std::size_t count) {
        state_->screenHost->OnInputSnapshot();
        for (std::size_t index = 0; index < count; ++index) {
            const CancellationToken token = state_->runtimeCancellation.Token();
            const Runtime::FixedStepContext context{++state_->runtimeSimulationTick, Duration::FromNanoseconds(16'666'667), token};
            const Result<void> advanced = state_->runtimeScene.OnFixedUpdate(context);
            if (advanced.HasError())
                throw std::runtime_error(advanced.ErrorValue().message);
            state_->screenHost->OnFixedUpdate(context.simulationTick, 1.0 / 60.0);
        }
    }

    std::optional<Math::Vec3> FullEditorUiTestHost::FirstViewportObjectPosition() const noexcept {
        const Editor::EditorViewportSceneView scene = state_->viewportScene.View();
        if (scene.instances.empty())
            return std::nullopt;
        return Math::TransformPoint(scene.instances.front().localToWorld, {});
    }

    std::optional<Math::Vec3> FullEditorUiTestHost::FirstAuthoringObjectPosition() const noexcept {
        const std::optional<Runtime::RuntimeSceneView> scene = state_->runtimeScene.ActiveScene();
        if (!scene)
            return std::nullopt;
        for (std::size_t slot = 0; slot < scene->SlotCount(); ++slot) {
            const std::optional<Runtime::RuntimeEntityView> entity = scene->EntityAt(slot);
            if (entity && entity->primitiveMesh != nullptr && entity->primitiveMesh->has_value())
                return entity->localTransform->translation;
        }
        return std::nullopt;
    }

    std::uint64_t FullEditorUiTestHost::ViewportDocumentRevision() const noexcept {
        return state_->viewportScene.Revision().value;
    }

    std::string FullEditorUiTestHost::BuildDiagnosticText() const {
        const std::optional<BuildOutputSnapshot> snapshot = state_->buildOutput.SnapshotIfChanged(0);
        std::string result;
        if (snapshot) {
            for (const BuildOutputRecord &record : snapshot->records)
                result += record.stage + ": " + record.message + "\n";
        }
        return result;
    }

    Editor::GuiRouteKind FullEditorUiTestHost::ActiveRoute() const noexcept {
        return state_->screenHost->ActiveRoute().kind;
    }

    const std::filesystem::path &FullEditorUiTestHost::ProjectsRoot() const noexcept {
        return state_->projectsRoot;
    }

    bool FullEditorUiTestHost::WasRouteDrawn(const Editor::GuiRouteKind route) const noexcept {
        return RouteDrawCount(route) != 0;
    }

    std::size_t FullEditorUiTestHost::RouteDrawCount(const Editor::GuiRouteKind route) const noexcept {
        return static_cast<std::size_t>(std::ranges::count(state_->drawnRoutes, route));
    }

    Editor::GuiScreenHost &FullEditorUiTestHost::Screens() noexcept {
        return *state_->screenHost;
    }

    void FullEditorUiTestHost::DispatchMenuInvocationOnNextFrame(Editor::EditorMenuInvocation invocation) {
        state_->pendingMenuInvocation = std::move(invocation);
    }

    bool FullEditorUiTestHost::BeginAssetImport(const std::filesystem::path &source) {
        auto *const modal = dynamic_cast<Editor::AssetImportModal *>(state_->modals.TopModal());
        if (modal == nullptr)
            return false;
        const CancellationToken cancellation;
        return modal->BeginImport({source}, state_->screenHost->CurrentProjectRoot(), cancellation).HasValue();
    }

    bool FullEditorUiTestHost::ImportFirstPendingAsset() {
        auto *const modal = dynamic_cast<Editor::AssetImportModal *>(state_->modals.TopModal());
        if (modal == nullptr || modal->Snapshot().items.empty())
            return false;
        const CancellationToken cancellation;
        return modal->ImportSingleItem(0, cancellation).HasValue();
    }

    bool FullEditorUiTestHost::ResolvePendingAssetConflict() {
        auto *const modal = dynamic_cast<Editor::AssetImportModal *>(state_->modals.TopModal());
        if (modal == nullptr || !modal->HasPendingConflicts())
            return false;
        modal->ResolveCurrentConflict(Editor::AssetImportModal::ConflictChoice::Rename, false);
        return modal->IsImportComplete();
    }

    Input::InputRouter &FullEditorUiTestHost::Input() noexcept {
        return state_->input;
    }

    Runtime::CameraProjection FullEditorUiTestHost::ViewportProjection() const noexcept {
        return state_->viewportScene.View().camera.projection;
    }

    bool FullEditorUiTestHost::RendererReady() const noexcept {
        return state_->viewportRenderer.IsReady();
    }

    std::string_view FullEditorUiTestHost::RendererName() const noexcept {
        return state_->surface.RendererName();
    }
}  // namespace Horo::Tests
