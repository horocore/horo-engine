#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace Math = Math;

    const ErrorCodeDescriptor InjectedFilesystemFailure{
        .domain = ErrorDomainId{"test.content_browser"},
        .code = ErrorCode{"injected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected Content Browser filesystem failure.",
    };

    void AppendU32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift & 0xffU));
    }

    void AppendFloat(std::vector<std::uint8_t> &bytes, const float value) {
        std::uint32_t raw{};
        std::memcpy(&raw, &value, sizeof(raw));
        AppendU32(bytes, raw);
    }

    void WriteTestMeshPayload(const std::filesystem::path &path) {
        std::vector<std::uint8_t> bytes;
        AppendU32(bytes, Assets::MeshEditorPayloadSchemaVersion);
        AppendU32(bytes, 3);
        AppendU32(bytes, 1);
        for (const float value : {-0.5F, 0.0F, -0.5F, 0.5F, 1.0F, 0.5F})
            AppendFloat(bytes, value);
        AppendU32(bytes, 36);
        AppendU32(bytes, 0);
        AppendU32(bytes, 0);
        for (const float value : {-0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F})
            AppendFloat(bytes, value);
        AppendU32(bytes, 3);
        AppendU32(bytes, 0);
        AppendU32(bytes, 1);
        AppendU32(bytes, 2);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    class SyncFailingFilesystem final : public DurableFileSystem {
    public:
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    const std::string_view ownerMetadata) override {
            return native_.TryAcquireExclusive(path, ownerMetadata);
        }

        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            ++atomicReplaceCalls;
            if (failReplace)
                return Result<void>::Failure(MakeError(InjectedFilesystemFailure));
            return native_.AtomicReplace(prepared, destination);
        }

        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (failSync)
                return Result<void>::Failure(MakeError(InjectedFilesystemFailure));
            return native_.SyncDirectory(path);
        }

        bool failSync{false};
        bool failReplace{false};
        std::size_t atomicReplaceCalls{0};

    private:
        NativeDurableFileSystem native_;
    };

    class PreviewTestImporter final : public Assets::IAssetImporter {
    public:
        [[nodiscard]] Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &,
                                                                 const CancellationToken &) const override {
            return Result<Assets::PreparedAssetImport>::Success({});
        }
    };

    class PreviewTestProvider final : public Assets::IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<Assets::AssetPreviewImage> GeneratePreview(const Assets::AssetPreviewInput &input,
                                                                        const CancellationToken &) const override {
            ++calls;
            return Result<Assets::AssetPreviewImage>::Success({
                .width = input.width,
                .height = input.height,
                .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(input.width) * input.height * 4U, 0x80),
            });
        }

        mutable std::atomic<std::uint32_t> calls{};
    };

    void WritePreviewTestAsset(const std::filesystem::path &assetPath) {
        std::filesystem::create_directories(assetPath.parent_path());
        std::ofstream payload(assetPath, std::ios::binary);
        payload << "preview-payload";
        std::ofstream metadata(assetPath.string() + ".meta", std::ios::binary);
        metadata << R"({"sourceFile":"sample.preview","type":"core.mesh"})";
    }

    void RegisterPreviewTestProvider(Assets::AssetImporterCatalog &catalog, const std::shared_ptr<PreviewTestProvider> &previewProvider) {
        auto type = Assets::AssetTypeId::Parse("core.mesh");
        REQUIRE(type.HasValue());
        REQUIRE(catalog
                    .Register(Assets::AssetImporterContribution{
                        .contributionId = "test.preview",
                        .packageId = "test.package",
                        .moduleId = "test.module",
                        .moduleVersion = "1.0.0",
                        .version = "1.0.0",
                        .fileExtensions = {"preview"},
                        .assetTypes = {std::move(type).Value()},
                        .strategy = std::make_shared<PreviewTestImporter>(),
                        .previewProvider = previewProvider,
                        .previewFallback = Assets::AssetPreviewFallback::Mesh,
                    })
                    .HasValue());
    }

    void WaitForPreview(EditorWorkspaceController &controller) {
        for (std::size_t attempt = 0; attempt < 100'000 && (controller.ViewModel().contentBrowser.entries.empty() ||
                                                            !controller.ViewModel().contentBrowser.entries.front().previewImage.IsValid());
             ++attempt) {
            controller.UpdateContentBrowser();
            std::this_thread::yield();
        }
    }

    struct PreviewWorkspaceFixture final {
        std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /  // NOSONAR(cpp:S5443): Unique test-only path; no untrusted input.
            std::format("horo-workspace-preview-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::shared_ptr<PreviewTestProvider> previewProvider = std::make_shared<PreviewTestProvider>();
        Assets::AssetImporterCatalog catalog;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> publishedCatalog;
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        std::unique_ptr<EditorWorkspaceController> controller;

        PreviewWorkspaceFixture() {
            WritePreviewTestAsset(projectRoot / "assets" / "sample.horoasset");
            RegisterPreviewTestProvider(catalog, previewProvider);
            auto published = catalog.Publish();
            REQUIRE(published.HasValue());
            publishedCatalog = std::move(published).Value();
            REQUIRE(runtimeScene.Startup(cancellation.Token()).HasValue());
            controller = std::make_unique<EditorWorkspaceController>(projectRoot, runtimeScene, Assets::AssetRegistrySnapshot{},
                                                                     EditorWorkspaceDependencies{.importerCatalog = publishedCatalog.get(),
                                                                                                 .jobs = &jobs});
        }

        ~PreviewWorkspaceFixture() {
            std::error_code cleanupError;
            std::filesystem::remove_all(projectRoot, cleanupError);
        }
    };

    class TestWorkspaceController final {
    public:
        explicit TestWorkspaceController(std::string projectRoot = "test-project", DiagnosticSourceNavigator diagnosticNavigator = {})
            : controller_(projectRoot, runtimeScene_, {},
                          EditorWorkspaceDependencies{.diagnosticSourceNavigator = std::move(diagnosticNavigator)}) {
            REQUIRE((runtimeScene_.Startup(cancellation_.Token()).HasValue()));
            PumpLifecycleCommit();
        }

        void ProcessCommand(const EditorWorkspaceViewCommandData &command) {
            controller_.ProcessCommand(command);
            PumpLifecycleCommit();
        }

        [[nodiscard]] const EditorWorkspaceViewModel &ViewModel() const noexcept {
            return controller_.ViewModel();
        }

        [[nodiscard]] EditorDataBus &DataBus() noexcept {
            return controller_.DataBus();
        }

        [[nodiscard]] ViewportRevision CurrentViewportRevision() const noexcept {
            return controller_.CurrentViewportRevision();
        }

        [[nodiscard]] const EditorViewportSceneSnapshot &ViewportScene() const noexcept {
            return controller_.ViewportScene();
        }

        void RefreshAssets(const Assets::AssetRegistrySnapshot &snapshot) {
            controller_.RefreshAssets(snapshot);
        }

        void UpdateContentBrowser() {
            controller_.UpdateContentBrowser();
        }

    private:
        void PumpLifecycleCommit() {
            const Runtime::FrameContext context{1, {}, 0.0, 0, {}, false, cancellation_.Token()};
            REQUIRE((runtimeScene_.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue()));
            controller_.SynchronizeRuntimeScenePreview();
        }

        Runtime::RuntimeSceneService runtimeScene_;
        CancellationSource cancellation_;
        EditorWorkspaceController controller_;
    };

    TEST_CASE("Gameplay behavior creation requests validate Lua and native kinds", "[unit][editor][behavior]") {
        const std::string destination = std::filesystem::absolute("project/assets/scripts").string();
        for (const GameplayBehaviorKind kind : {GameplayBehaviorKind::Lua, GameplayBehaviorKind::Native}) {
            const CreateGameplayBehaviorRequest request{.destination = destination, .baseName = "PlayerBehavior", .kind = kind};
            REQUIRE((ValidateCreateGameplayBehaviorRequest(request).HasValue()));
        }
    }

    TEST_CASE("Content browser schedules preview providers and reuses cached images", "[unit][editor][assets][preview]") {
        PreviewWorkspaceFixture fixture;
        WaitForPreview(*fixture.controller);
        REQUIRE(fixture.controller->ViewModel().contentBrowser.entries.size() == 1);
        REQUIRE(fixture.controller->ViewModel().contentBrowser.entries.front().previewImage.IsValid());
        REQUIRE(fixture.previewProvider->calls.load() == 1);

        fixture.controller->ProcessCommand({.command = EditorWorkspaceViewCommand::RefreshContentBrowser});
        fixture.controller->UpdateContentBrowser();
        fixture.controller->UpdateContentBrowser();
        WaitForPreview(*fixture.controller);
        REQUIRE(fixture.controller->ViewModel().contentBrowser.entries.front().previewImage.IsValid());
        REQUIRE(fixture.previewProvider->calls.load() == 1);
    }

    TEST_CASE("Gameplay behavior creation requests reject invalid destinations and names", "[unit][editor][behavior]") {
        for (const std::string destination : {"", "project/assets/scripts", "./project/assets/scripts"}) {
            const CreateGameplayBehaviorRequest request{.destination = destination,
                                                        .baseName = "PlayerBehavior",
                                                        .kind = GameplayBehaviorKind::Lua};
            const Result<void> result = ValidateCreateGameplayBehaviorRequest(request);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "workspace.gameplay_behavior.invalid_request");
        }
        for (const std::string name : {"", "dir/name", "dir\\\\name", "behavior.lua", ".", "..", "CON", "COM1", "LPT1", "com1.txt",
                                       "lpt1.lua", "name ", "name.", "bad<name"}) {
            const CreateGameplayBehaviorRequest request{.destination = "/project/assets/scripts",
                                                        .baseName = name,
                                                        .kind = GameplayBehaviorKind::Lua};
            const Result<void> result = ValidateCreateGameplayBehaviorRequest(request);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "workspace.gameplay_behavior.invalid_request");
        }
        const CreateGameplayBehaviorRequest controlCharacterRequest{.destination = "/project/assets/scripts",
                                                                    .baseName = std::string{"bad\x01name"},
                                                                    .kind = GameplayBehaviorKind::Lua};
        const Result<void> controlCharacterResult = ValidateCreateGameplayBehaviorRequest(controlCharacterRequest);
        REQUIRE(controlCharacterResult.HasError());
        REQUIRE(controlCharacterResult.ErrorValue().code.Value() == "workspace.gameplay_behavior.invalid_request");
    }

    TEST_CASE("Gameplay behavior creation command writes requested lua source and refreshes browser", "[unit][editor][behavior]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-create-lua-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scriptsDirectory = projectRoot / "assets/scripts";
        std::filesystem::create_directories(scriptsDirectory);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = scriptsDirectory.string();
        controller.ProcessCommand(navigate);

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CreateLuaBehavior;
        command.gameplayBehaviorRequest = CreateGameplayBehaviorRequest{
            .destination = scriptsDirectory.string(),
            .baseName = "PlayerBehavior",
            .kind = GameplayBehaviorKind::Lua,
        };
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Loading ||
                 controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Ready));
        REQUIRE((std::filesystem::is_regular_file(scriptsDirectory / "PlayerBehavior.horo_script")));
        REQUIRE((std::filesystem::is_regular_file(scriptsDirectory / "PlayerBehavior.horo_script.meta")));

        controller.UpdateContentBrowser();
        controller.UpdateContentBrowser();

        REQUIRE((controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Ready));
        const auto created = std::ranges::find_if(controller.ViewModel().contentBrowser.entries, [](const ContentBrowserEntry &entry) {
            return entry.displayName == "PlayerBehavior" && entry.kind == ContentBrowserEntryKind::Asset;
        });
        REQUIRE((created != controller.ViewModel().contentBrowser.entries.end()));
        REQUIRE((created->absolutePath == std::filesystem::weakly_canonical(scriptsDirectory / "PlayerBehavior.horo_script").string()));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Save Persists And Reopens The Default Scene", "[unit][editor][persistence]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));

        {
            EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE((controller.ViewModel().objects.empty()));
            REQUIRE((!controller.ViewModel().isDirty));

            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::CreatePrimitive;
            create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
            controller.ProcessCommand(create);
            REQUIRE((controller.ViewModel().objects.size() == 1));
            REQUIRE((controller.ViewModel().isDirty));

            controller.UpdateAutosave(60.0F, 1);
            const std::filesystem::path canonicalProjectRoot = std::filesystem::weakly_canonical(projectRoot);
            const std::filesystem::path canonicalScenePath = std::filesystem::weakly_canonical(scenePath);
            auto recovery = InspectProjectSceneRecovery(canonicalProjectRoot, canonicalScenePath);
            INFO((recovery.HasError() ? recovery.ErrorValue().message : std::string{}));
            REQUIRE((recovery.HasValue()));
            REQUIRE((recovery.Value().has_value()));
            REQUIRE((recovery.Value()->objects.size() == 1));
            auto canonicalBeforeSave = LoadProjectDefaultScene(projectRoot);
            REQUIRE((canonicalBeforeSave.HasValue()));
            REQUIRE((canonicalBeforeSave.Value().has_value()));
            REQUIRE((canonicalBeforeSave.Value()->objects.empty()));

            EditorWorkspaceViewCommandData save;
            save.command = EditorWorkspaceViewCommand::SaveScene;
            controller.ProcessCommand(save);
            REQUIRE((!controller.ViewModel().isDirty));
            recovery = InspectProjectSceneRecovery(canonicalProjectRoot, canonicalScenePath);
            REQUIRE((recovery.HasValue()));
            REQUIRE((!recovery.Value().has_value()));
        }

        {
            EditorWorkspaceController reopened{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE((reopened.ViewModel().objects.size() == 1));
            REQUIRE((reopened.ViewModel().objects.front().name == "Box"));
            REQUIRE((!reopened.ViewModel().isDirty));
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Save As Changes Identity While Save Copy As Does Not", "[unit][editor][persistence][save-as]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-save-as-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        const std::filesystem::path copyPath = projectRoot / "assets/scenes/copy.horo";
        const std::filesystem::path saveAsPath = projectRoot / "assets/scenes/renamed.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().has_value()));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(scenePath)));

        EditorWorkspaceViewCommandData saveCopy;
        saveCopy.command = EditorWorkspaceViewCommand::SaveSceneCopyAs;
        saveCopy.stringPayload = copyPath.string();
        controller.ProcessCommand(saveCopy);
        REQUIRE((std::filesystem::exists(copyPath)));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(scenePath)));

        EditorWorkspaceViewCommandData saveAs;
        saveAs.command = EditorWorkspaceViewCommand::SaveSceneAs;
        saveAs.stringPayload = saveAsPath.string();
        controller.ProcessCommand(saveAs);
        REQUIRE((std::filesystem::exists(saveAsPath)));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(saveAsPath)));

        controller.ProcessCommand(create);
        EditorWorkspaceViewCommandData save;
        save.command = EditorWorkspaceViewCommand::SaveScene;
        controller.ProcessCommand(save);
        REQUIRE((!controller.ViewModel().isDirty));

        auto renamedFingerprint = InspectProjectSceneFingerprint(projectRoot, saveAsPath);
        auto copyFingerprint = InspectProjectSceneFingerprint(projectRoot, copyPath);
        REQUIRE((renamedFingerprint.HasValue()));
        REQUIRE((copyFingerprint.HasValue()));
        REQUIRE((renamedFingerprint.Value() != copyFingerprint.Value()));

        {
            std::ofstream external(saveAsPath, std::ios::binary | std::ios::trunc);
            external << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }
        EditorWorkspaceViewCommandData reload;
        reload.command = EditorWorkspaceViewCommand::ReloadExternalScene;
        controller.ProcessCommand(reload);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(saveAsPath)));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Classifies And Explicitly Restores Scene Recovery", "[unit][editor][persistence][recovery]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));

        {
            EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::CreatePrimitive;
            create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
            controller.ProcessCommand(create);
            controller.UpdateAutosave(60.0F, 1);
            REQUIRE((std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));
        }

        {
            EditorWorkspaceController reopened{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE((reopened.ViewModel().recoveryAvailable));
            REQUIRE((reopened.ViewModel().objects.empty()));
            REQUIRE((!reopened.ViewModel().isDirty));

            EditorWorkspaceViewCommandData restore;
            restore.command = EditorWorkspaceViewCommand::RestoreSceneRecovery;
            reopened.ProcessCommand(restore);
            REQUIRE((!reopened.ViewModel().recoveryAvailable));
            REQUIRE((reopened.ViewModel().objects.size() == 1));
            REQUIRE((reopened.ViewModel().isDirty));
            REQUIRE((std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));

            EditorWorkspaceViewCommandData save;
            save.command = EditorWorkspaceViewCommand::SaveScene;
            reopened.ProcessCommand(save);
            REQUIRE((!reopened.ViewModel().isDirty));
            REQUIRE((!std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));
        }

        auto canonical = LoadProjectDefaultScene(projectRoot);
        REQUIRE((canonical.HasValue()));
        REQUIRE((canonical.Value().has_value()));
        REQUIRE((canonical.Value()->objects.size() == 1));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Autosave Coalesces Unchanged State And Retries With Backoff", "[unit][editor][persistence][recovery]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-autosave-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        SyncFailingFilesystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);

        controller.UpdateAutosave(59.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 0));
        controller.UpdateAutosave(1.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 1));
        controller.UpdateAutosave(60.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 1));

        controller.ProcessCommand(create);
        files.failReplace = true;
        controller.UpdateAutosave(60.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 2));
        files.failReplace = false;
        controller.UpdateAutosave(29.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 2));
        controller.UpdateAutosave(1.0F, 1);
        REQUIRE((files.atomicReplaceCalls == 3));

        auto recovery =
            InspectProjectSceneRecovery(std::filesystem::weakly_canonical(projectRoot), std::filesystem::weakly_canonical(scenePath));
        REQUIRE((recovery.HasValue()));
        REQUIRE((recovery.Value().has_value()));
        REQUIRE((recovery.Value()->objects.size() == 2));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Blocks Conflicting Save Until Reload Or Explicit Overwrite", "[unit][editor][persistence][conflict]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-conflict-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);
        controller.UpdateAutosave(60.0F, 1);
        {
            std::ofstream external(scenePath, std::ios::binary | std::ios::trunc);
            external << "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
        }

        EditorWorkspaceViewCommandData save;
        save.command = EditorWorkspaceViewCommand::SaveScene;
        controller.ProcessCommand(save);
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().sceneExternalConflict));
        auto captured = controller.CaptureExternalSceneComparison();
        REQUIRE((captured.HasValue()));
        auto comparison = LoadSceneDocumentComparison(std::move(captured).Value());
        REQUIRE((comparison.HasValue()));
        REQUIRE((comparison.Value().removedFromDisk == 1));
        REQUIRE((comparison.Value().addedOnDisk == 0));
        REQUIRE((comparison.Value().modified == 0));
        REQUIRE((std::filesystem::path{comparison.Value().absoluteScenePath}.is_absolute()));

        EditorWorkspaceViewCommandData reload;
        reload.command = EditorWorkspaceViewCommand::ReloadExternalScene;
        controller.ProcessCommand(reload);
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().sceneExternalConflict));
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((!std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));

        controller.ProcessCommand(create);
        {
            std::ofstream external(scenePath, std::ios::binary | std::ios::trunc);
            external << "{\n\"schemaVersion\": 1,\n\"objects\": []\n}\n";
        }
        controller.ProcessCommand(save);
        REQUIRE((controller.ViewModel().sceneExternalConflict));

        EditorWorkspaceViewCommandData overwrite;
        overwrite.command = EditorWorkspaceViewCommand::OverwriteExternalScene;
        controller.ProcessCommand(overwrite);
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().sceneExternalConflict));
        auto canonical = LoadProjectDefaultScene(std::filesystem::weakly_canonical(projectRoot));
        REQUIRE((canonical.HasValue()));
        REQUIRE((canonical.Value().has_value()));
        REQUIRE((canonical.Value()->objects.size() == 1));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Reports Clean Scene Changes From The Background Watch", "[unit][editor][persistence][watch]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-watch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        {
            std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
            metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
            std::ofstream scene(scenePath, std::ios::binary);
            scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        JobSystem jobs(JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8});
        {
            EditorWorkspaceController controller{projectRoot,
                                                 runtimeScene,
                                                 {},
                                                 {.mutations = &mutations, .durableFiles = &files, .jobs = &jobs}};
            REQUIRE((!controller.ViewModel().isDirty));
            REQUIRE((!controller.ViewModel().sceneExternalConflict));

            {
                std::ofstream external(scenePath, std::ios::binary | std::ios::trunc);
                external << "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
            }
            controller.UpdateExternalSceneWatch(1.0F);
            for (std::size_t attempt = 0; attempt < 100'000 && !controller.ViewModel().sceneExternalConflict; ++attempt) {
                controller.UpdateExternalSceneWatch(0.001F);
                std::this_thread::yield();
            }
            REQUIRE((controller.ViewModel().sceneExternalConflict));
            REQUIRE((!controller.ViewModel().isDirty));
            auto captured = controller.CaptureExternalSceneComparison();
            REQUIRE((captured.HasValue()));
            const auto comparison = LoadSceneDocumentComparison(std::move(captured).Value());
            REQUIRE((comparison.HasValue()));
            REQUIRE((!comparison.Value().HasDifferences()));
            REQUIRE((std::filesystem::path{comparison.Value().absoluteScenePath}.is_absolute()));

            EditorWorkspaceViewCommandData reload;
            reload.command = EditorWorkspaceViewCommand::ReloadExternalScene;
            controller.ProcessCommand(reload);
            REQUIRE((!controller.ViewModel().sceneExternalConflict));
            REQUIRE((!controller.ViewModel().isDirty));
        }
        jobs.Shutdown(ShutdownPolicy::Drain);

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Published Asset Registry Revisions Refresh The Content Browser Projection", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path meshesDirectory = projectRoot / "assets/Meshes";
        std::filesystem::create_directories(meshesDirectory);
        {
            std::ofstream payload(meshesDirectory / "arrow_bow3.horoasset", std::ios::binary);
            payload << "asset";
        }

        TestWorkspaceController controller{projectRoot.string()};
        Assets::AssetRegistry registry;
        auto assetId = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
        auto assetType = Assets::AssetTypeId::Parse("core.mesh");
        auto sourcePath = ProjectPath::Parse("assets/Meshes/arrow_bow3.horoasset");
        auto metadataPath = ProjectPath::Parse("assets/Meshes/arrow_bow3.horoasset.horo");
        REQUIRE((assetId.HasValue()));
        REQUIRE((assetType.HasValue()));
        REQUIRE((sourcePath.HasValue()));
        REQUIRE((metadataPath.HasValue()));
        const auto published = registry.Publish({Assets::AssetRecord{
            .id = std::move(assetId).Value(),
            .type = std::move(assetType).Value(),
            .sourcePath = std::move(sourcePath).Value(),
            .metadataPath = std::move(metadataPath).Value(),
        }});
        REQUIRE((published.status == Assets::AssetRegistryBuildStatus::Complete));

        controller.RefreshAssets(registry.Snapshot());

        REQUIRE((controller.ViewModel().assetRegistryRevision == published.publishedRevision));
        REQUIRE((controller.ViewModel().contentBrowser.entries.size() == 1));
        REQUIRE((controller.ViewModel().contentBrowser.entries[0].kind == ContentBrowserEntryKind::Directory));
        REQUIRE((controller.ViewModel().contentBrowser.entries[0].displayName == "Meshes"));
        REQUIRE((std::filesystem::path{controller.ViewModel().contentBrowser.entries[0].absolutePath}.is_absolute()));

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = meshesDirectory.string();
        controller.ProcessCommand(navigate);
        REQUIRE((controller.ViewModel().contentBrowser.absoluteCurrentPath == std::filesystem::weakly_canonical(meshesDirectory).string()));
        REQUIRE((controller.ViewModel().contentBrowser.entries.size() == 1));
        REQUIRE((controller.ViewModel().contentBrowser.entries[0].displayName == "arrow_bow3"));
        REQUIRE((std::filesystem::path{controller.ViewModel().contentBrowser.entries[0].absolutePath}.is_absolute()));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Asset drop command creates selects and atomically undoes a registered mesh", "[unit][editor][asset-drop]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-drop-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path meshesDirectory = projectRoot / "assets/Meshes";
        std::filesystem::create_directories(meshesDirectory);
        const std::filesystem::path meshPath = meshesDirectory / "chair.horoasset";
        WriteTestMeshPayload(meshPath);
        const std::filesystem::path brokenPath = meshesDirectory / "broken.horoasset";
        {
            std::ofstream broken(brokenPath, std::ios::binary);
            broken << "broken";
        }

        Assets::AssetRegistry registry;
        const auto assetId = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
        const auto assetType = Assets::AssetTypeId::Parse("core.mesh");
        const auto sourcePath = ProjectPath::Parse("assets/Meshes/chair.horoasset");
        const auto metadataPath = ProjectPath::Parse("assets/Meshes/chair.horoasset.horo");
        const auto brokenId = Assets::AssetId::Parse("11112233-4455-6677-8899-aabbccddeeff");
        const auto brokenSourcePath = ProjectPath::Parse("assets/Meshes/broken.horoasset");
        const auto brokenMetadataPath = ProjectPath::Parse("assets/Meshes/broken.horoasset.horo");
        REQUIRE((assetId.HasValue() && assetType.HasValue() && sourcePath.HasValue() && metadataPath.HasValue() && brokenId.HasValue() &&
                 brokenSourcePath.HasValue() && brokenMetadataPath.HasValue()));
        const Assets::AssetRegistryBuildReport published = registry.Publish(
            {Assets::AssetRecord{assetId.Value(), assetType.Value(), sourcePath.Value(), metadataPath.Value()},
             Assets::AssetRecord{brokenId.Value(), assetType.Value(), brokenSourcePath.Value(), brokenMetadataPath.Value()}});
        REQUIRE((published.status == Assets::AssetRegistryBuildStatus::Complete));

        TestWorkspaceController controller{projectRoot.string()};
        controller.RefreshAssets(registry.Snapshot());
        std::vector<NotificationEvent> notifications;
        const auto subscription = controller.DataBus().Subscribe<NotificationEvent>([&](const NotificationEvent &event) {
            notifications.push_back(event);
        });
        static_cast<void>(subscription);

        EditorWorkspaceViewCommandData brokenDrop;
        brokenDrop.command = EditorWorkspaceViewCommand::InstantiateAsset;
        brokenDrop.assetSceneDrop = AssetSceneDropRequest{
            .assetId = brokenId.Value().ToString(),
            .assetType = assetType.Value().Value(),
            .target = AssetSceneDropTarget::HierarchyRoot,
            .documentRevision = controller.ViewModel().documentRevision,
        };
        controller.ProcessCommand(brokenDrop);
        CHECK(controller.ViewModel().objects.size() == 1);
        REQUIRE((!notifications.empty()));
        CHECK(notifications.back().severity == NotificationSeverity::Error);

        EditorWorkspaceViewCommandData drop;
        drop.command = EditorWorkspaceViewCommand::InstantiateAsset;
        drop.assetSceneDrop = AssetSceneDropRequest{
            .assetId = assetId.Value().ToString(),
            .assetType = assetType.Value().Value(),
            .target = AssetSceneDropTarget::Viewport,
            .normalizedX = 0.5F,
            .normalizedY = 0.5F,
            .aspect = 1.0F,
            .documentRevision = controller.ViewModel().documentRevision,
        };
        controller.ProcessCommand(drop);

        REQUIRE((controller.ViewModel().objects.size() == 2));
        CHECK(controller.ViewModel().objects.back().name == "chair");
        CHECK(controller.ViewModel().primarySelection == controller.ViewModel().objects.back().id);
        CHECK(controller.ViewModel().isDirty);
        REQUIRE((!notifications.empty()));
        CHECK(notifications.back().severity == NotificationSeverity::Success);

        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK(controller.ViewModel().objects.size() == 1);
        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::RedoScene});
        REQUIRE((controller.ViewModel().objects.size() == 2));
        CHECK(controller.ViewModel().objects.back().name == "chair");

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Refresh Exposes Loading And Reconciles Deleted Navigation Targets", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-refresh-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path deletedDirectory = projectRoot / "assets/Deleted";
        std::filesystem::create_directories(deletedDirectory);

        TestWorkspaceController controller{projectRoot.string()};
        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = deletedDirectory.string();
        controller.ProcessCommand(navigate);
        REQUIRE((controller.ViewModel().contentBrowserCanNavigateBack));

        std::error_code removeError;
        std::filesystem::remove_all(deletedDirectory, removeError);
        REQUIRE_FALSE(removeError);

        EditorWorkspaceViewCommandData refresh;
        refresh.command = EditorWorkspaceViewCommand::RefreshContentBrowser;
        controller.ProcessCommand(refresh);
        REQUIRE((controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Loading));

        controller.UpdateContentBrowser();
        REQUIRE((controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Loading));
        controller.UpdateContentBrowser();
        REQUIRE((controller.ViewModel().contentBrowser.loadState == ContentBrowserLoadState::Ready));
        REQUIRE((controller.ViewModel().contentBrowser.absoluteCurrentPath ==
                 std::filesystem::weakly_canonical(projectRoot / "assets").string()));
        REQUIRE_FALSE(controller.ViewModel().contentBrowserCanNavigateBack);
        REQUIRE_FALSE(controller.ViewModel().contentBrowserCanNavigateForward);

        std::filesystem::remove_all(projectRoot, removeError);
    }

    TEST_CASE("Content Browser Rename And Delete Preserve Registry Consistency", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-mutation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path meshesDirectory = projectRoot / "assets/Meshes";
        std::filesystem::create_directories(meshesDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        const std::filesystem::path source = meshesDirectory / "crate.horoasset";
        {
            std::ofstream payload(source, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(source.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "00112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        Assets::AssetRegistry registry;
        const auto initial = Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
        REQUIRE((initial.HasValue()));
        REQUIRE((initial.Value().status == Assets::AssetRegistryBuildStatus::Complete));
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = meshesDirectory.string();
        controller.ProcessCommand(navigate);

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry;
        rename.stringPayload = source.string();
        rename.secondaryStringPayload = "hero_crate.horoasset";
        controller.ProcessCommand(rename);
        const std::filesystem::path renamed = meshesDirectory / "hero_crate.horoasset";
        REQUIRE((!std::filesystem::exists(source)));
        REQUIRE((std::filesystem::is_regular_file(renamed)));
        REQUIRE((std::filesystem::is_regular_file(renamed.string() + ".horo")));
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((registry.Snapshot().Records().size() == 1));
        REQUIRE((registry.Snapshot().Records()[0].sourcePath.String() == "assets/Meshes/hero_crate.horoasset"));

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteContentBrowserEntry;
        remove.stringPayload = renamed.string();
        controller.ProcessCommand(remove);
        REQUIRE((!std::filesystem::exists(renamed)));
        REQUIRE((!std::filesystem::exists(renamed.string() + ".horo")));
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((registry.Snapshot().Records().empty()));
        const std::filesystem::path trashRoot = projectRoot / ".horo/local/trash";
        REQUIRE((std::filesystem::is_directory(trashRoot)));
        const std::filesystem::path trashEntry = std::filesystem::directory_iterator(trashRoot)->path();
        REQUIRE((std::filesystem::is_regular_file(trashEntry / renamed.filename())));
        REQUIRE((std::filesystem::is_regular_file(trashEntry / (renamed.filename().string() + ".horo"))));
        std::ifstream manifestStream(trashEntry / "trash.json");
        const nlohmann::json manifest = nlohmann::json::parse(manifestStream);
        const std::filesystem::path recordedOriginal = manifest.at("originalAbsolutePath").get<std::string>();
        REQUIRE((recordedOriginal == std::filesystem::weakly_canonical(renamed)));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Copy And Move Preserve Asset Identity Contracts", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-copy-move-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path sourceDirectory = projectRoot / "assets/Meshes";
        const std::filesystem::path targetDirectory = projectRoot / "assets/Target";
        std::filesystem::create_directories(sourceDirectory);
        std::filesystem::create_directories(targetDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        const std::filesystem::path source = sourceDirectory / "crate.horoasset";
        {
            std::ofstream payload(source, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(source.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "00112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        Assets::AssetRegistry registry;
        const auto initial = Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
        REQUIRE((initial.HasValue()));
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = sourceDirectory.string();
        controller.ProcessCommand(navigate);

        EditorWorkspaceViewCommandData duplicate;
        duplicate.command = EditorWorkspaceViewCommand::DuplicateContentBrowserAsset;
        duplicate.stringPayload = source.string();
        controller.ProcessCommand(duplicate);
        const std::filesystem::path duplicatePath = sourceDirectory / "crate (1).horoasset";
        REQUIRE((std::filesystem::is_regular_file(duplicatePath)));
        REQUIRE((std::filesystem::is_regular_file(duplicatePath.string() + ".horo")));
        const Assets::AssetRegistrySnapshot afterDuplicate = registry.Snapshot();
        const Assets::AssetRecord *originalRecord = afterDuplicate.FindByPath("assets/Meshes/crate.horoasset");
        const Assets::AssetRecord *duplicateRecord = afterDuplicate.FindByPath("assets/Meshes/crate (1).horoasset");
        REQUIRE((originalRecord != nullptr));
        REQUIRE((duplicateRecord != nullptr));
        REQUIRE((originalRecord->id != duplicateRecord->id));
        const Assets::AssetId originalId = originalRecord->id;
        const Assets::AssetId duplicateId = duplicateRecord->id;

        EditorWorkspaceViewCommandData copy;
        copy.command = EditorWorkspaceViewCommand::CopyContentBrowserAsset;
        copy.stringPayload = source.string();
        controller.ProcessCommand(copy);
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::Copy));

        EditorWorkspaceViewCommandData paste;
        paste.command = EditorWorkspaceViewCommand::PasteContentBrowserAsset;
        paste.stringPayload = targetDirectory.string();
        controller.ProcessCommand(paste);
        const std::filesystem::path copiedPath = targetDirectory / "crate.horoasset";
        REQUIRE((std::filesystem::is_regular_file(copiedPath)));
        const Assets::AssetRegistrySnapshot afterCopy = registry.Snapshot();
        const Assets::AssetRecord *copiedRecord = afterCopy.FindByPath("assets/Target/crate.horoasset");
        REQUIRE((copiedRecord != nullptr));
        REQUIRE((copiedRecord->id != originalId));
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::Copy));

        EditorWorkspaceViewCommandData cut;
        cut.command = EditorWorkspaceViewCommand::CutContentBrowserAsset;
        cut.stringPayload = source.string();
        controller.ProcessCommand(cut);
        paste.stringPayload = targetDirectory.string();
        controller.ProcessCommand(paste);
        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::Move));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists"));

        cut.stringPayload = duplicatePath.string();
        controller.ProcessCommand(cut);
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::Move));

        paste.stringPayload = targetDirectory.string();
        controller.ProcessCommand(paste);
        const std::filesystem::path movedPath = targetDirectory / "crate (1).horoasset";
        REQUIRE((!std::filesystem::exists(duplicatePath)));
        REQUIRE((std::filesystem::is_regular_file(movedPath)));
        const Assets::AssetRegistrySnapshot afterMove = registry.Snapshot();
        const Assets::AssetRecord *movedRecord = afterMove.FindByPath("assets/Target/crate (1).horoasset");
        REQUIRE((movedRecord != nullptr));
        REQUIRE((movedRecord->id == duplicateId));
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::None));
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));

        const std::filesystem::path dragCopyDirectory = projectRoot / "assets/DragCopy";
        const std::filesystem::path dragMoveDirectory = projectRoot / "assets/DragMove";
        std::filesystem::create_directories(dragCopyDirectory);
        std::filesystem::create_directories(dragMoveDirectory);

        EditorWorkspaceViewCommandData dragCopy;
        dragCopy.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        dragCopy.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = source.string(),
            .absoluteDestinationDirectory = dragCopyDirectory.string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        controller.ProcessCommand(dragCopy);
        const Assets::AssetRegistrySnapshot afterDragCopy = registry.Snapshot();
        const Assets::AssetRecord *dragCopyRecord = afterDragCopy.FindByPath("assets/DragCopy/crate.horoasset");
        REQUIRE((dragCopyRecord != nullptr));
        REQUIRE((dragCopyRecord->id != originalId));
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::None));

        EditorWorkspaceViewCommandData dragMove;
        dragMove.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        dragMove.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = movedPath.string(),
            .absoluteDestinationDirectory = dragMoveDirectory.string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        controller.ProcessCommand(dragMove);
        REQUIRE((!std::filesystem::exists(movedPath)));
        const Assets::AssetRegistrySnapshot afterDragMove = registry.Snapshot();
        const Assets::AssetRecord *dragMoveRecord = afterDragMove.FindByPath("assets/DragMove/crate (1).horoasset");
        REQUIRE((dragMoveRecord != nullptr));
        REQUIRE((dragMoveRecord->id == duplicateId));
        REQUIRE((controller.ViewModel().contentBrowserClipboard.mode == ContentBrowserClipboardMode::None));

        EditorWorkspaceViewCommandData relativeDrag;
        relativeDrag.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        relativeDrag.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = "assets/Meshes/crate.horoasset",
            .absoluteDestinationDirectory = dragCopyDirectory.string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        controller.ProcessCommand(relativeDrag);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.invalid_target"));

        EditorWorkspaceViewCommandData createFolder;
        createFolder.command = EditorWorkspaceViewCommand::CreateContentBrowserFolder;
        createFolder.stringPayload = sourceDirectory.string();
        createFolder.secondaryStringPayload = "Generated";
        controller.ProcessCommand(createFolder);
        REQUIRE((std::filesystem::is_directory(sourceDirectory / "Generated")));

        navigate.stringPayload = targetDirectory.string();
        controller.ProcessCommand(navigate);
        REQUIRE((controller.ViewModel().contentBrowserCanNavigateBack));
        EditorWorkspaceViewCommandData navigateBack;
        navigateBack.command = EditorWorkspaceViewCommand::NavigateContentBrowserBack;
        controller.ProcessCommand(navigateBack);
        REQUIRE((controller.ViewModel().contentBrowser.absoluteCurrentPath == std::filesystem::weakly_canonical(sourceDirectory).string()));
        REQUIRE((controller.ViewModel().contentBrowserCanNavigateForward));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Move Rolls Back When Directory Durability Fails", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-move-rollback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path sourceDirectory = projectRoot / "assets/Source";
        const std::filesystem::path targetDirectory = projectRoot / "assets/Target";
        std::filesystem::create_directories(sourceDirectory);
        std::filesystem::create_directories(targetDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        const std::filesystem::path source = sourceDirectory / "rollback.horoasset";
        {
            std::ofstream payload(source, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(source.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "20112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        Assets::AssetRegistry registry;
        REQUIRE((Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit).HasValue()));
        SyncFailingFilesystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = sourceDirectory.string();
        controller.ProcessCommand(navigate);

        files.failSync = true;
        EditorWorkspaceViewCommandData move;
        move.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        move.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = source.string(),
            .absoluteDestinationDirectory = targetDirectory.string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        controller.ProcessCommand(move);

        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((std::filesystem::is_regular_file(source.string() + ".horo")));
        REQUIRE_FALSE(std::filesystem::exists(targetDirectory / source.filename()));
        REQUIRE((registry.Snapshot().FindByPath("assets/Source/rollback.horoasset") != nullptr));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.move_failed"));

        files.failSync = false;
        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Rejects Unsafe Portable Paths And Companion Sets", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-path-hardening-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path sourceDirectory = projectRoot / "assets/Source";
        const std::filesystem::path targetDirectory = projectRoot / "assets/Target";
        const std::filesystem::path outsideDirectory = projectRoot.parent_path() / (projectRoot.filename().string() + "-outside");
        std::filesystem::create_directories(sourceDirectory);
        std::filesystem::create_directories(targetDirectory);
        std::filesystem::create_directories(outsideDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        std::filesystem::path source = sourceDirectory / "crate.horoasset";
        {
            std::ofstream payload(source, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(source.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "30112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        Assets::AssetRegistry registry;
        REQUIRE((Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit).HasValue()));
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = sourceDirectory.string();
        controller.ProcessCommand(navigate);

        {
            std::ofstream collision(targetDirectory / "CRATE.HOROASSET", std::ios::binary);
            collision << "collision";
        }
        EditorWorkspaceViewCommandData move;
        move.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        move.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = source.string(),
            .absoluteDestinationDirectory = targetDirectory.string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        controller.ProcessCommand(move);
        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists"));
        std::filesystem::remove(targetDirectory / "CRATE.HOROASSET");

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry;
        rename.stringPayload = source.string();
        rename.secondaryStringPayload = "çatı.horoasset";
        controller.ProcessCommand(rename);
        source = sourceDirectory / "çatı.horoasset";
        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((std::filesystem::is_regular_file(source.string() + ".horo")));

        {
            std::ofstream companionCollision(sourceDirectory / "safe.horoasset.horo");
            companionCollision << "collision";
        }
        rename.stringPayload = source.string();
        rename.secondaryStringPayload = "safe.horoasset";
        controller.ProcessCommand(rename);
        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists"));
        std::filesystem::remove(sourceDirectory / "safe.horoasset.horo");

        const std::filesystem::path identitySidecar = source.string() + ".horo";
        const std::filesystem::path heldSidecar = sourceDirectory / "held-sidecar";
        std::filesystem::rename(identitySidecar, heldSidecar);
        EditorWorkspaceViewCommandData copy;
        copy.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        copy.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = source.string(),
            .absoluteDestinationDirectory = targetDirectory.string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        controller.ProcessCommand(copy);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.companion_invalid"));
        REQUIRE_FALSE(std::filesystem::exists(targetDirectory / source.filename()));
        std::filesystem::rename(heldSidecar, identitySidecar);

        EditorWorkspaceViewCommandData createFolder;
        createFolder.command = EditorWorkspaceViewCommand::CreateContentBrowserFolder;
        createFolder.stringPayload = sourceDirectory.string();
        createFolder.secondaryStringPayload = "CON";
        controller.ProcessCommand(createFolder);
        std::error_code reservedNameError;
        REQUIRE_FALSE(std::filesystem::exists(sourceDirectory / "CON", reservedNameError));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.invalid_name"));
        createFolder.secondaryStringPayload = "unsafe.";
        controller.ProcessCommand(createFolder);
        std::error_code trailingDotError;
        REQUIRE_FALSE(std::filesystem::exists(sourceDirectory / "unsafe.", trailingDotError));

        std::error_code symlinkError;
        const std::filesystem::path escape = projectRoot / "assets/Escape";
        std::filesystem::create_directory_symlink(outsideDirectory, escape, symlinkError);
        if (!symlinkError) {
            copy.contentBrowserTransfer->absoluteDestinationDirectory = escape.string();
            controller.ProcessCommand(copy);
            REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.invalid_target"));
            REQUIRE(std::filesystem::is_empty(outsideDirectory));
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
        std::filesystem::remove_all(outsideDirectory, cleanupError);
    }

    TEST_CASE("Diagnostic source navigation rejects files outside the project", "[unit][editor][diagnostics]") {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            ("horo-diagnostic-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path projectRoot = base / "project";
        const std::filesystem::path outsideSource = base / "outside.cpp";
        std::filesystem::create_directories(projectRoot);
        {
            std::ofstream source(outsideSource);
            source << "int main() {}\n";
        }

        TestWorkspaceController controller{projectRoot.string()};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = outsideSource.string(), .line = 1};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid"));

        std::error_code cleanupError;
        std::filesystem::remove_all(base, cleanupError);
    }

    TEST_CASE("Diagnostic source navigation preserves a validated line and column", "[unit][editor][diagnostics]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-diagnostic-location-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path sourcePath = projectRoot / "assets/shaders/material.glsl";
        std::filesystem::create_directories(sourcePath.parent_path());
        {
            std::ofstream source(sourcePath);
            source << "void main() {}\n";
        }

        std::optional<DiagnosticSourceRequest> navigated;
        TestWorkspaceController controller{projectRoot.string(), [&navigated](const DiagnosticSourceRequest &source) {
            navigated = source;
            return true;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
        command.diagnosticSource = DiagnosticSourceRequest{.absolutePath = sourcePath.string(), .line = 27, .column = 9};
        controller.ProcessCommand(command);

        REQUIRE(navigated.has_value());
        REQUIRE((std::filesystem::weakly_canonical(std::filesystem::path{navigated->absolutePath}) ==
                 std::filesystem::weakly_canonical(sourcePath)));
        REQUIRE((navigated->line == 27));
        REQUIRE((navigated->column == 9));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Delete Succeeds When Registry Rebuild Is Degraded", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-registry-rollback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path assetDirectory = projectRoot / "assets/Source";
        std::filesystem::create_directories(assetDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        const std::filesystem::path source = assetDirectory / "stable.horoasset";
        {
            std::ofstream payload(source, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(source.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "40112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        Assets::AssetRegistry registry;
        const auto initial = Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
        REQUIRE((initial.HasValue()));
        REQUIRE((initial.Value().status == Assets::AssetRegistryBuildStatus::Complete));
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = assetDirectory.string();
        controller.ProcessCommand(navigate);

        const std::filesystem::path corrupt = assetDirectory / "corrupt.horoasset";
        {
            std::ofstream payload(corrupt, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(corrupt.string() + ".horo");
            metadata << "{ invalid";
        }

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry;
        rename.stringPayload = source.string();
        rename.secondaryStringPayload = "renamed.horoasset";
        controller.ProcessCommand(rename);
        REQUIRE((std::filesystem::is_regular_file(source)));
        REQUIRE((std::filesystem::is_regular_file(source.string() + ".horo")));
        REQUIRE_FALSE(std::filesystem::exists(assetDirectory / "renamed.horoasset"));
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.registry_failed"));

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteContentBrowserEntry;
        remove.stringPayload = source.string();
        controller.ProcessCommand(remove);
        REQUIRE_FALSE(std::filesystem::exists(source));
        REQUIRE_FALSE(std::filesystem::exists(source.string() + ".horo"));
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((registry.Snapshot().FindByPath("assets/Source/stable.horoasset") == nullptr));
        REQUIRE((std::filesystem::is_regular_file(corrupt)));
        REQUIRE((std::filesystem::is_regular_file(corrupt.string() + ".horo")));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content Browser Delete Removes Non Registry Files During Degraded Rebuild", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-script-delete-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path assetDirectory = projectRoot / "assets/Scripts";
        std::filesystem::create_directories(assetDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");

        const std::filesystem::path trackedAsset = assetDirectory / "tracked.horoasset";
        {
            std::ofstream payload(trackedAsset, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(trackedAsset.string() + ".horo");
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "50112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }
        const std::filesystem::path corruptAsset = assetDirectory / "corrupt.horoasset";
        {
            std::ofstream payload(corruptAsset, std::ios::binary);
            payload << "asset";
        }
        {
            std::ofstream metadata(corruptAsset.string() + ".horo");
            metadata << "{ invalid";
        }
        const std::filesystem::path script = assetDirectory / "NewBehavior4.horo_script";
        {
            std::ofstream payload(script, std::ios::binary);
            payload << "script";
        }
        {
            std::ofstream metadata(script.string() + ".meta");
            metadata << "{}";
        }

        Assets::AssetRegistry registry;
        const auto initial = Assets::RebuildAssetRegistry(registry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
        REQUIRE((initial.HasValue()));
        REQUIRE((initial.Value().status == Assets::AssetRegistryBuildStatus::Degraded));
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot,
                                             runtimeScene,
                                             registry.Snapshot(),
                                             {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateContentBrowser;
        navigate.stringPayload = assetDirectory.string();
        controller.ProcessCommand(navigate);

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteContentBrowserEntry;
        remove.stringPayload = script.string();
        controller.ProcessCommand(remove);
        REQUIRE_FALSE(std::filesystem::exists(script));
        REQUIRE_FALSE(std::filesystem::exists(script.string() + ".meta"));
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((registry.Snapshot().FindByPath("assets/Scripts/tracked.horoasset") != nullptr));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Moving An Active Panel Across Areas Updates Its Runtime Placement", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.hierarchy";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 2, 1};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.panelDockAreas.at("horo.hierarchy") == WorkspaceDockArea::Bottom));
        const auto slot = viewModel.activityBarLayout.FindSlot("horo.hierarchy");
        REQUIRE((slot.has_value()));
        REQUIRE((*slot == ActivityBarSlot{ActivityBarRail::Left, 2, 1}));
        REQUIRE((viewModel.activityBarLayout.ItemAt(ActivityBarRail::Left, 2, 0) == "horo.global_dock"));
    }

    TEST_CASE("Replacing A Target Area Preserves The Displaced Panels Placement", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.activeBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.panelDockAreas.at("horo.global_dock") == WorkspaceDockArea::Bottom));
    }

    TEST_CASE("Dropping Into Bottom Right Splits The Full Bottom Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 2;
        command.stringPayload = "horo.inspector";
        command.bottomDockSlot = BottomDockSlot::Right;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Split));
        REQUIRE((viewModel.activeBottomPanelId.empty()));
        REQUIRE((viewModel.activeBottomLeftPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomRightPanelId == "horo.inspector"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Clicking A Split Panel Expands It To The Full Bottom Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData clickCommand;
        clickCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        clickCommand.targetIndex = 2;
        clickCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(clickCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

    TEST_CASE("Dropping A Left Rail Icon Into Bottom Right Moves It To The Right Rail", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData fullCommand;
        fullCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        fullCommand.targetIndex = 2;
        fullCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(fullCommand);

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.hierarchy";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Split));
        REQUIRE((viewModel.activeBottomLeftPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeBottomRightPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Places The Viewport In The Document Top Rail By Default", "[unit][editor]") {
        TestWorkspaceController controller;

        REQUIRE(
            (controller.ViewModel().activityBarLayout.FindSlot("horo.viewport") == ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
    }

    TEST_CASE("Dropping Into The Lower Half Splits The Left Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 0;
        command.stringPayload = "horo.inspector";
        command.sideDockSlot = SideDockSlot::Bottom;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeRightPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));
    }

    TEST_CASE("Dropping Into The Lower Half Splits The Right Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 1;
        command.stringPayload = "horo.hierarchy";
        command.sideDockSlot = SideDockSlot::Bottom;
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.rightDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeRightPanelId.empty()));
        REQUIRE((viewModel.activeRightTopPanelId == "horo.inspector"));
        REQUIRE((viewModel.activeRightBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Right, 1, 0}));
    }

    TEST_CASE("Clicking A Split Side Panel Expands It To The Full Dock", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData clickCommand;
        clickCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        clickCommand.targetIndex = 0;
        clickCommand.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(clickCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
    }

    TEST_CASE("Moving One Half Away Expands The Remaining Side Panel", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData moveCommand;
        moveCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        moveCommand.targetIndex = 1;
        moveCommand.stringPayload = "horo.inspector";
        moveCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 0, 0};
        controller.ProcessCommand(moveCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
    }

    TEST_CASE("Reordering An Active Icon Moves Its Panel And Activates A Source Fallback", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData closeFallback;
        closeFallback.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        closeFallback.targetIndex = 2;
        closeFallback.stringPayload = std::string{};
        controller.ProcessCommand(closeFallback);

        EditorWorkspaceViewCommandData placeFallback;
        placeFallback.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        placeFallback.stringPayload = "horo.global_dock";
        placeFallback.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 0, 1};
        controller.ProcessCommand(placeFallback);

        EditorWorkspaceViewCommandData openBottom;
        openBottom.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        openBottom.targetIndex = 0;
        openBottom.stringPayload = "horo.inspector";
        openBottom.sideDockSlot = SideDockSlot::Bottom;
        openBottom.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(openBottom);

        EditorWorkspaceViewCommandData moveActiveIcon;
        moveActiveIcon.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        moveActiveIcon.stringPayload = "horo.hierarchy";
        moveActiveIcon.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 1};
        controller.ProcessCommand(moveActiveIcon);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId != "horo.inspector"));
        REQUIRE((viewModel.panelDockAreas.at("horo.hierarchy") == WorkspaceDockArea::Left));
        REQUIRE((viewModel.panelDockAreas.at("horo.global_dock") == WorkspaceDockArea::Left));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Left, 1, 1}));
    }

    TEST_CASE("Reordering The Only Bottom Icon Does Not Leave A Half Empty Split", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        command.stringPayload = "horo.global_dock";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

    TEST_CASE("Dropping On A Side Merge Target Expands The Panel Without Moving Its Icon", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 0;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.sideDockSlot = SideDockSlot::Bottom;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData mergeCommand;
        mergeCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        mergeCommand.targetIndex = 0;
        mergeCommand.stringPayload = "horo.hierarchy";
        controller.ProcessCommand(mergeCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftTopPanelId.empty()));
        REQUIRE((viewModel.activeLeftBottomPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.hierarchy") == ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));

        EditorWorkspaceViewCommandData replaceFullCommand;
        replaceFullCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        replaceFullCommand.targetIndex = 0;
        replaceFullCommand.stringPayload = "horo.inspector";
        controller.ProcessCommand(replaceFullCommand);

        REQUIRE((viewModel.leftDockMode == SideDockMode::Full));
        REQUIRE((viewModel.activeLeftPanelId == "horo.inspector"));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Left, 1, 0}));
    }

    TEST_CASE("Dropping On The Bottom Merge Target Preserves Its Activity Group", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData splitCommand;
        splitCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitCommand.targetIndex = 2;
        splitCommand.stringPayload = "horo.inspector";
        splitCommand.bottomDockSlot = BottomDockSlot::Right;
        splitCommand.activityBarSlot = ActivityBarSlot{ActivityBarRail::Right, 2, 0};
        controller.ProcessCommand(splitCommand);

        EditorWorkspaceViewCommandData mergeCommand;
        mergeCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        mergeCommand.targetIndex = 2;
        mergeCommand.stringPayload = "horo.global_dock";
        controller.ProcessCommand(mergeCommand);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.bottomDockMode == BottomDockMode::Full));
        REQUIRE((viewModel.activeBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.global_dock") == ActivityBarSlot{ActivityBarRail::Left, 2, 0}));
        REQUIRE((viewModel.activityBarLayout.FindSlot("horo.inspector") == ActivityBarSlot{ActivityBarRail::Right, 2, 0}));
    }

    TEST_CASE("Reordering An Active Bottom Panel To The Left Does Not Render It Twice", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        command.stringPayload = "horo.global_dock";
        command.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 1, 0};
        controller.ProcessCommand(command);

        const auto &viewModel = controller.ViewModel();
        REQUIRE((viewModel.leftDockMode == SideDockMode::Split));
        REQUIRE((viewModel.activeLeftTopPanelId == "horo.hierarchy"));
        REQUIRE((viewModel.activeLeftBottomPanelId == "horo.global_dock"));
        REQUIRE((viewModel.activeBottomPanelId.empty()));
        REQUIRE((viewModel.activeBottomLeftPanelId.empty()));
        REQUIRE((viewModel.activeBottomRightPanelId.empty()));
    }

    TEST_CASE("Scene Commands Publish Committed Events And Drive Undo Redo State", "[unit][editor]") {
        TestWorkspaceController controller;
        std::vector<SceneDocumentChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&events](const SceneDocumentChangedEvent &event) {
            events.push_back(event);
        });
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().canUndo));

        EditorWorkspaceViewCommandData add;
        add.command = EditorWorkspaceViewCommand::CreatePrimitive;
        add.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(add);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1 && events.back().kind == DocumentChangeKind::Created));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canRedo));
        REQUIRE((events.size() == 2 && events.back().kind == DocumentChangeKind::Undone));

        EditorWorkspaceViewCommandData redo;
        redo.command = EditorWorkspaceViewCommand::RedoScene;
        controller.ProcessCommand(redo);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((events.size() == 3 && events.back().kind == DocumentChangeKind::Redone));
    }

    TEST_CASE("Editor Visibility And Lock Commands Project State And Block Locked Edits", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject original = controller.ViewModel().objects.front();
        std::vector<NotificationEvent> notifications;
        const auto subscription = controller.DataBus().Subscribe<NotificationEvent>([&notifications](const NotificationEvent &event) {
            notifications.push_back(event);
        });
        static_cast<void>(subscription);

        EditorWorkspaceViewCommandData hide;
        hide.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
        hide.objectPayload = original.id;
        hide.editorStatePayload = SceneObjectEditorState{.visible = false, .locked = false};
        controller.ProcessCommand(hide);
        REQUIRE_FALSE((controller.ViewModel().objects.front().editorState.visible));
        REQUIRE_FALSE((controller.ViewModel().objects.front().effectivelyVisible));
        REQUIRE((notifications.empty()));

        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE((controller.ViewModel().objects.front().editorState == SceneObjectEditorState{}));

        EditorWorkspaceViewCommandData lock;
        lock.command = EditorWorkspaceViewCommand::UpdateObjectEditorState;
        lock.objectPayload = original.id;
        lock.editorStatePayload = SceneObjectEditorState{.visible = true, .locked = true};
        controller.ProcessCommand(lock);
        REQUIRE((controller.ViewModel().objects.front().effectivelyLocked));
        REQUIRE((notifications.empty()));

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
        rename.objectPayload = original.id;
        rename.stringPayload = "Blocked Rename";
        controller.ProcessCommand(rename);
        REQUIRE((controller.ViewModel().objects.front().name == original.name));
        REQUIRE((notifications.size() == 1));
        REQUIRE((notifications.front().severity == NotificationSeverity::Warning));

        lock.editorStatePayload = SceneObjectEditorState{};
        controller.ProcessCommand(lock);
        REQUIRE_FALSE((controller.ViewModel().objects.front().effectivelyLocked));
        REQUIRE((notifications.size() == 1));
    }

    TEST_CASE("Catalog Creation Selects The Result And Honors The Requested Parent", "[unit][editor]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData createRoot;
        createRoot.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createRoot.primitivePayload = Runtime::PrimitiveId{"primitive.object.empty"};
        controller.ProcessCommand(createRoot);
        const SceneObject root = controller.ViewModel().objects.back();
        REQUIRE((root.kind == SceneObjectKind::GameObject));
        REQUIRE((!root.parent.has_value()));
        REQUIRE((controller.ViewModel().primarySelection == root.id));

        EditorWorkspaceViewCommandData createCamera;
        createCamera.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createCamera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
        createCamera.objectPayload = root.id;
        controller.ProcessCommand(createCamera);
        const SceneObject camera = controller.ViewModel().objects.back();
        REQUIRE((camera.kind == SceneObjectKind::Camera));
        REQUIRE((camera.parent == root.id));
        REQUIRE((controller.ViewModel().primarySelection == camera.id));
        REQUIRE((controller.ViewModel().hierarchyRevealObject == camera.id));
    }

    TEST_CASE("Stable Selection Drives Inspector Projection And Reconciles After Delete", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        std::vector<SelectionChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);
        REQUIRE((controller.ViewModel().primarySelection == object));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength > 0.0F));
        REQUIRE((events.size() == 1 && events.back().kind == SelectionChangeKind::ObjectsChanged));

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteObject;
        remove.objectPayload = object;
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((events.size() == 2 && events.back().kind == SelectionChangeKind::Cleared));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 1));
        REQUIRE((controller.ViewModel().objects.front().id == object));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((events.size() == 2));
        static_cast<void>(subscription);
    }

    TEST_CASE("Batch Delete Uses One Selection Snapshot History Entry And Snackbar", "[unit][editor]") {
        TestWorkspaceController controller;
        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);
        const std::vector<SceneObject> before = controller.ViewModel().objects;
        REQUIRE((before.size() == 2));

        std::vector<NotificationEvent> notifications;
        const auto notificationSubscription =
            controller.DataBus().Subscribe<NotificationEvent>([&notifications](const NotificationEvent &event) {
            notifications.push_back(event);
        });
        static_cast<void>(notificationSubscription);

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectSelection = ObjectSelectionRequest{.objects = {before[0].id, before[1].id}, .primary = before[1].id};
        controller.ProcessCommand(select);

        const DocumentRevision beforeDeleteRevision = controller.ViewModel().documentRevision;
        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
        remove.objectSelection = ObjectSelectionRequest{
            .objects = {before[0].id, before[1].id, before[0].id},
            .primary = before[1].id,
        };
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((controller.ViewModel().selectedObjects.empty()));
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((!controller.ViewModel().primarySelectionWorldBounds.has_value()));
        REQUIRE((controller.ViewModel().documentRevision.value == beforeDeleteRevision.value + 1));
        REQUIRE((notifications.size() == 1));
        REQUIRE((notifications.front().severity == NotificationSeverity::Success));
        REQUIRE((notifications.front().message.find("2") != std::string::npos));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 2));
        REQUIRE((controller.ViewModel().objects[0].id == before[0].id));
        REQUIRE((controller.ViewModel().objects[1].id == before[1].id));

        EditorWorkspaceViewCommandData partialRemove;
        partialRemove.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
        partialRemove.objectSelection = ObjectSelectionRequest{
            .objects = {before[0].id, before[1].id, SceneObjectId{999999}},
            .primary = before[1].id,
        };
        controller.ProcessCommand(partialRemove);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((notifications.size() == 2));
        REQUIRE((notifications.back().severity == NotificationSeverity::Warning));
        REQUIRE((notifications.back().message.find("2") != std::string::npos));
        REQUIRE((notifications.back().message.find("1") != std::string::npos));

        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.size() == 2));

        EditorWorkspaceViewCommandData redo;
        redo.command = EditorWorkspaceViewCommand::RedoScene;
        controller.ProcessCommand(redo);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((notifications.size() == 2));
    }

    TEST_CASE("Viewport Picking Uses The Authoritative Selection Model", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        std::vector<SelectionChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SelectionChangedEvent>([&events](const SelectionChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData hit;
        hit.command = EditorWorkspaceViewCommand::PickViewport;
        hit.viewportPickPayload = ViewportPickRequest{.normalizedX = 0.5F, .normalizedY = 0.5F, .aspect = 1.0F};
        controller.ProcessCommand(hit);
        REQUIRE((controller.ViewModel().primarySelection == object));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength > 0.0F));
        REQUIRE((events.size() == 1 && events.back().kind == SelectionChangeKind::ObjectsChanged));

        EditorWorkspaceViewCommandData miss;
        miss.command = EditorWorkspaceViewCommand::PickViewport;
        miss.viewportPickPayload = ViewportPickRequest{.normalizedX = 0.0F, .normalizedY = 0.0F, .aspect = 1.0F};
        controller.ProcessCommand(miss);
        REQUIRE((!controller.ViewModel().primarySelection.has_value()));
        REQUIRE((controller.ViewportScene().instances.front().presentation.tintStrength == 0.0F));
        REQUIRE((events.size() == 2 && events.back().kind == SelectionChangeKind::Cleared));
        static_cast<void>(subscription);
    }

    TEST_CASE("Transform Commands Update The Document Projection Viewport And History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform original = controller.ViewModel().objects.front().localTransform;
        std::vector<SceneDocumentChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&events](const SceneDocumentChangedEvent &event) {
            events.push_back(event);
        });

        const Math::Transform edited{
            .translation = {2.0F, 3.0F, -1.0F},
            .rotation = Math::Quaternion::FromEulerRadians({0.2F, -0.3F, 0.4F}),
            .scale = {1.5F, 2.0F, 0.5F},
        };
        EditorWorkspaceViewCommandData transform;
        transform.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        transform.objectPayload = object;
        transform.transformPayload = edited;
        controller.ProcessCommand(transform);

        REQUIRE((controller.ViewModel().objects.front().localTransform == edited));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1));
        REQUIRE((events.back().kind == DocumentChangeKind::TransformChanged));
        REQUIRE((events.back().affectedObjects == std::vector{object}));
        const Math::Vec3 worldOrigin = Math::TransformPoint(controller.ViewportScene().instances.front().localToWorld, {});
        REQUIRE((worldOrigin == edited.translation));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().localTransform == original));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((events.size() == 2 && events.back().kind == DocumentChangeKind::Undone));
        static_cast<void>(subscription);
    }

    TEST_CASE("Viewport Navigation Updates Only The Editor Camera Authority", "[unit][editor]") {
        TestWorkspaceController controller;
        const EditorViewportCamera before = controller.ViewportScene().camera;
        const DocumentRevision documentRevision = controller.ViewModel().documentRevision;
        std::vector<ViewportChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<ViewportChangedEvent>([&events](const ViewportChangedEvent &event) {
            events.push_back(event);
        });

        EditorWorkspaceViewCommandData navigate;
        navigate.command = EditorWorkspaceViewCommand::NavigateViewport;
        navigate.viewportNavigationPayload = EditorViewportNavigationDelta{.yawRadians = 0.2F, .moveForward = 0.5F};
        controller.ProcessCommand(navigate);

        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{1}));
        REQUIRE((controller.ViewportScene().camera.IsValid()));
        REQUIRE((controller.ViewportScene().camera.position != before.position));
        REQUIRE((controller.ViewModel().documentRevision == documentRevision));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((!controller.ViewModel().canUndo));
        REQUIRE((events.size() == 1 && events.front().kind == ViewportChangeKind::CameraMoved));
        static_cast<void>(subscription);
    }

    TEST_CASE("Viewport Focus Treats An Empty Selection As No Interaction", "[unit][editor]") {
        TestWorkspaceController controller;
        const EditorViewportCamera before = controller.ViewportScene().camera;
        const ViewportRevision viewportRevision = controller.CurrentViewportRevision();
        std::vector<ViewportChangedEvent> events;
        auto subscription = controller.DataBus().Subscribe<ViewportChangedEvent>([&events](const ViewportChangedEvent &event) {
            events.push_back(event);
        });

        REQUIRE_FALSE(controller.ViewModel().primarySelectionWorldBounds.has_value());
        EditorWorkspaceViewCommandData focus;
        focus.command = EditorWorkspaceViewCommand::FocusViewportSelection;
        focus.floatPayload = 1.0F;
        controller.ProcessCommand(focus);

        REQUIRE((controller.CurrentViewportRevision() == viewportRevision));
        REQUIRE((controller.ViewportScene().camera.position == before.position));
        REQUIRE((controller.ViewportScene().camera.target == before.target));
        REQUIRE(events.empty());
        static_cast<void>(subscription);
    }

    TEST_CASE("Gizmo Preview Is Transient And Commit Creates One Undoable Document Change", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform edited = controller.ViewModel().objects.front().localTransform;
        edited.translation = {1.0F, 2.0F, -0.5F};
        std::vector<SceneDocumentChangedEvent> documentEvents;
        std::vector<ViewportChangedEvent> viewportEvents;
        auto documentSubscription =
            controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&documentEvents](const SceneDocumentChangedEvent &event) {
            documentEvents.push_back(event);
        });
        auto viewportSubscription =
            controller.DataBus().Subscribe<ViewportChangedEvent>([&viewportEvents](const ViewportChangedEvent &event) {
            viewportEvents.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = object;
        preview.transformPayload = edited;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().objects.front().localTransform != edited));
        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        const Math::Vec3 previewOrigin = Math::TransformPoint(controller.ViewportScene().instances.front().localToWorld, {});
        REQUIRE((previewOrigin == edited.translation));
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((Math::TransformPoint(*controller.ViewModel().primarySelectionPreviewWorldTransform, {}) == edited.translation));
        REQUIRE((documentEvents.empty()));
        REQUIRE((viewportEvents.size() == 1 && viewportEvents.front().kind == ViewportChangeKind::ScenePreviewChanged));

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.objectPayload = object;
        commit.transformPayload = edited;
        controller.ProcessCommand(commit);
        REQUIRE((controller.ViewModel().objects.front().localTransform == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().isDirty && controller.ViewModel().canUndo));
        REQUIRE((documentEvents.size() == 1 && documentEvents.front().kind == DocumentChangeKind::TransformChanged));
        REQUIRE((!controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{2}));
        REQUIRE((viewportEvents.size() == 2 && viewportEvents.back().kind == ViewportChangeKind::ScenePreviewChanged));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((!controller.ViewModel().isDirty));
        static_cast<void>(documentSubscription);
        static_cast<void>(viewportSubscription);
    }

    TEST_CASE("Inspector Rename Updates Projection Once And Remains Undoable", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const std::string originalName = controller.ViewModel().objects.front().name;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::UpdateObjectName;
        rename.objectPayload = object;
        rename.stringPayload = "Inspector Hero";
        controller.ProcessCommand(rename);

        REQUIRE((controller.ViewModel().objects.front().name == "Inspector Hero"));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(rename);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().name == originalName));
    }

    TEST_CASE("Inspector Camera Update Uses Typed Document History", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createCamera;
        createCamera.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createCamera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
        controller.ProcessCommand(createCamera);

        const SceneObject camera = controller.ViewModel().objects.back();
        REQUIRE((camera.kind == SceneObjectKind::Camera));
        REQUIRE((camera.components.camera.has_value()));
        const Runtime::CameraComponent original = *camera.components.camera;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        Runtime::CameraComponent edited = original;
        edited.projection = Runtime::CameraProjection::Orthographic;
        edited.orthographicHeight = 24.0F;
        edited.nearPlane = 0.5F;
        edited.farPlane = 5000.0F;

        EditorWorkspaceViewCommandData update;
        update.command = EditorWorkspaceViewCommand::UpdateCameraComponent;
        update.objectPayload = camera.id;
        update.cameraPayload = edited;
        controller.ProcessCommand(update);

        const auto updated = std::ranges::find(controller.ViewModel().objects, camera.id, &SceneObject::id);
        REQUIRE((updated != controller.ViewModel().objects.end()));
        REQUIRE((updated->components.camera == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(update);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        const auto restored = std::ranges::find(controller.ViewModel().objects, camera.id, &SceneObject::id);
        REQUIRE((restored != controller.ViewModel().objects.end()));
        REQUIRE((restored->components.camera == original));
    }

    TEST_CASE("Inspector Light Update Uses Typed Document History", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);

        const SceneObject light = controller.ViewModel().objects.back();
        REQUIRE((light.kind == SceneObjectKind::Light));
        REQUIRE((light.components.light.has_value()));
        const Runtime::LightComponent original = *light.components.light;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        Runtime::LightComponent edited = original;
        edited.kind = Runtime::LightKind::Spot;
        edited.color = {0.4F, 0.6F, 0.8F};
        edited.intensity = 5.0F;
        edited.range = 30.0F;
        edited.innerConeRadians = 0.25F;
        edited.outerConeRadians = 0.75F;

        EditorWorkspaceViewCommandData update;
        update.command = EditorWorkspaceViewCommand::UpdateLightComponent;
        update.objectPayload = light.id;
        update.lightPayload = edited;
        controller.ProcessCommand(update);

        const auto updated = std::ranges::find(controller.ViewModel().objects, light.id, &SceneObject::id);
        REQUIRE((updated != controller.ViewModel().objects.end()));
        REQUIRE((updated->components.light == edited));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().canUndo));

        controller.ProcessCommand(update);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        const auto restored = std::ranges::find(controller.ViewModel().objects, light.id, &SceneObject::id);
        REQUIRE((restored != controller.ViewModel().objects.end()));
        REQUIRE((restored->components.light == original));
    }

    TEST_CASE("Inspector Optional Components Add Update And Remove Through Typed History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;

        EditorWorkspaceViewCommandData addTrigger;
        addTrigger.command = EditorWorkspaceViewCommand::AddComponentToObject;
        addTrigger.objectPayload = object;
        addTrigger.componentTypePayload = ComponentType::TriggerVolume;
        controller.ProcessCommand(addTrigger);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume.has_value()));

        Runtime::TriggerVolumeComponent trigger = *controller.ViewModel().objects.front().components.triggerVolume;
        trigger.shape = Runtime::ColliderShapeType::Sphere;
        EditorWorkspaceViewCommandData updateTrigger;
        updateTrigger.command = EditorWorkspaceViewCommand::UpdateTriggerVolumeComponent;
        updateTrigger.objectPayload = object;
        updateTrigger.triggerVolumePayload = trigger;
        controller.ProcessCommand(updateTrigger);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume == trigger));

        EditorWorkspaceViewCommandData addAudio;
        addAudio.command = EditorWorkspaceViewCommand::AddComponentToObject;
        addAudio.objectPayload = object;
        addAudio.componentTypePayload = ComponentType::AudioSource;
        controller.ProcessCommand(addAudio);
        Runtime::AudioSourceComponent audio = *controller.ViewModel().objects.front().components.audioSource;
        audio.playback.gain = 1.5F;
        EditorWorkspaceViewCommandData updateAudio;
        updateAudio.command = EditorWorkspaceViewCommand::UpdateAudioSourceComponent;
        updateAudio.objectPayload = object;
        updateAudio.audioSourcePayload = audio;
        controller.ProcessCommand(updateAudio);
        REQUIRE((controller.ViewModel().objects.front().components.audioSource == audio));

        EditorWorkspaceViewCommandData removeTrigger;
        removeTrigger.command = EditorWorkspaceViewCommand::RemoveComponentFromObject;
        removeTrigger.objectPayload = object;
        removeTrigger.componentTypePayload = ComponentType::TriggerVolume;
        controller.ProcessCommand(removeTrigger);
        REQUIRE((!controller.ViewModel().objects.front().components.triggerVolume.has_value()));
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 5));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().components.triggerVolume == trigger));
    }

    TEST_CASE("Inspector Light Preview Updates Viewport Without Mutating Document", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);
        const SceneObject light = controller.ViewModel().objects.back();
        const Runtime::LightComponent original = *light.components.light;
        const DocumentRevision originalRevision = controller.ViewModel().documentRevision;

        Runtime::LightComponent edited = original;
        edited.intensity = 7.0F;
        edited.range = 35.0F;
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewLightComponent;
        preview.objectPayload = light.id;
        preview.lightPayload = edited;
        controller.ProcessCommand(preview);

        REQUIRE((controller.ViewModel().documentRevision == originalRevision));
        REQUIRE((controller.ViewModel().objects.back().components.light == original));
        REQUIRE((controller.ViewportScene().lights.back().intensity == 7.0F));
        REQUIRE((controller.ViewportScene().lights.back().range == 35.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().object == light.id));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == 7.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().light.range == 35.0F));

        EditorWorkspaceViewCommandData cancel;
        cancel.command = EditorWorkspaceViewCommand::CancelLightComponentPreview;
        controller.ProcessCommand(cancel);
        REQUIRE((controller.ViewModel().documentRevision == originalRevision));
        REQUIRE((controller.ViewportScene().lights.back().intensity == original.intensity));
        REQUIRE((controller.ViewportScene().lights.back().range == original.range));
        REQUIRE((controller.ViewModel().viewportLights.back().object == light.id));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == original.intensity));
        REQUIRE((controller.ViewModel().viewportLights.back().light.range == original.range));
    }

    TEST_CASE("Cancelling Gizmo Preview Restores The Exact Committed Projection", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform committedTransform = controller.ViewModel().objects.front().localTransform;
        const Math::Mat4 committedMatrix = controller.ViewportScene().instances.front().localToWorld;
        const DocumentRevision committedRevision = controller.ViewModel().documentRevision;
        std::vector<SceneDocumentChangedEvent> documentEvents;
        std::vector<ViewportChangedEvent> viewportEvents;
        auto documentSubscription =
            controller.DataBus().Subscribe<SceneDocumentChangedEvent>([&documentEvents](const SceneDocumentChangedEvent &event) {
            documentEvents.push_back(event);
        });
        auto viewportSubscription =
            controller.DataBus().Subscribe<ViewportChangedEvent>([&viewportEvents](const ViewportChangedEvent &event) {
            viewportEvents.push_back(event);
        });

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectPayload = object;
        controller.ProcessCommand(select);

        Math::Transform previewTransform = committedTransform;
        previewTransform.translation = {-2.0F, 0.5F, 1.0F};
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = object;
        preview.transformPayload = previewTransform;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));

        EditorWorkspaceViewCommandData cancel;
        cancel.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
        controller.ProcessCommand(cancel);

        REQUIRE((controller.ViewModel().objects.front().localTransform == committedTransform));
        REQUIRE((controller.ViewportScene().instances.front().localToWorld == committedMatrix));
        REQUIRE((!controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));
        REQUIRE((controller.ViewModel().documentRevision == committedRevision));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        REQUIRE((documentEvents.empty()));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{2}));
        REQUIRE((viewportEvents.size() == 2));
        static_cast<void>(documentSubscription);
        static_cast<void>(viewportSubscription);
    }

    TEST_CASE("Idle Frames Preserve Inspector Transform And Light Previews", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject mesh = controller.ViewModel().objects.front();
        EditorWorkspaceViewCommandData selectMesh;
        selectMesh.command = EditorWorkspaceViewCommand::SelectObject;
        selectMesh.objectPayload = mesh.id;
        controller.ProcessCommand(selectMesh);
        Math::Transform previewTransform = mesh.localTransform;
        previewTransform.translation = {3.0F, 2.0F, 1.0F};

        EditorWorkspaceViewCommandData transformPreview;
        transformPreview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        transformPreview.transformUpdates = std::vector{SceneObjectTransformUpdate{mesh.id, previewTransform}};
        controller.ProcessCommand(transformPreview);
        controller.ProcessCommand({});

        const auto meshInstance = std::ranges::find(controller.ViewportScene().instanceObjects, mesh.id);
        REQUIRE((meshInstance != controller.ViewportScene().instanceObjects.end()));
        const std::size_t meshIndex =
            static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), meshInstance));
        REQUIRE((Math::TransformPoint(controller.ViewportScene().instances[meshIndex].localToWorld, {}) == previewTransform.translation));
        REQUIRE((controller.ViewModel().primarySelectionPreviewWorldTransform.has_value()));

        EditorWorkspaceViewCommandData createLight;
        createLight.command = EditorWorkspaceViewCommand::CreatePrimitive;
        createLight.primitivePayload = Runtime::PrimitiveId{"primitive.object.light_point"};
        controller.ProcessCommand(createLight);
        const SceneObject light = controller.ViewModel().objects.back();
        Runtime::LightComponent previewLight = *light.components.light;
        previewLight.intensity = 9.0F;

        EditorWorkspaceViewCommandData lightPreview;
        lightPreview.command = EditorWorkspaceViewCommand::PreviewLightComponent;
        lightPreview.objectPayload = light.id;
        lightPreview.lightPayload = previewLight;
        controller.ProcessCommand(lightPreview);
        controller.ProcessCommand({});

        REQUIRE((controller.ViewportScene().lights.back().intensity == 9.0F));
        REQUIRE((controller.ViewModel().viewportLights.back().light.intensity == 9.0F));
    }

    TEST_CASE("Selection Change Cancels A Transient Inspector Transform Preview", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject first = controller.ViewModel().objects.front();

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);
        const SceneObject second = controller.ViewModel().objects.back();
        const DocumentRevision committedRevision = controller.ViewModel().documentRevision;
        const bool committedDirtyState = controller.ViewModel().isDirty;
        const bool committedUndoState = controller.ViewModel().canUndo;

        EditorWorkspaceViewCommandData selectFirst;
        selectFirst.command = EditorWorkspaceViewCommand::SelectObject;
        selectFirst.objectPayload = first.id;
        controller.ProcessCommand(selectFirst);

        Math::Transform previewTransform = first.localTransform;
        previewTransform.translation = {4.0F, 2.0F, -1.0F};
        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.objectPayload = first.id;
        preview.transformPayload = previewTransform;
        controller.ProcessCommand(preview);

        const auto firstInstance = std::ranges::find(controller.ViewportScene().instanceObjects, first.id);
        REQUIRE((firstInstance != controller.ViewportScene().instanceObjects.end()));
        const std::size_t firstIndex =
            static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), firstInstance));
        REQUIRE((Math::TransformPoint(controller.ViewportScene().instances[firstIndex].localToWorld, {}) == previewTransform.translation));

        EditorWorkspaceViewCommandData selectSecond;
        selectSecond.command = EditorWorkspaceViewCommand::SelectObject;
        selectSecond.objectPayload = second.id;
        controller.ProcessCommand(selectSecond);

        REQUIRE((controller.ViewModel().primarySelection == second.id));
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        REQUIRE(
            (Math::TransformPoint(controller.ViewportScene().instances[firstIndex].localToWorld, {}) == first.localTransform.translation));
        REQUIRE((controller.ViewModel().documentRevision == committedRevision));
        REQUIRE((controller.ViewModel().isDirty == committedDirtyState));
        REQUIRE((controller.ViewModel().canUndo == committedUndoState));
    }

    TEST_CASE("No Op Gizmo Commit Clears Preview Without Creating History", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObjectId object = controller.ViewModel().objects.front().id;
        const Math::Transform committedTransform = controller.ViewModel().objects.front().localTransform;
        Math::Transform movedTransform = committedTransform;
        movedTransform.translation = {3.0F, 0.0F, 0.0F};

        EditorWorkspaceViewCommandData previewMoved;
        previewMoved.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        previewMoved.objectPayload = object;
        previewMoved.transformPayload = movedTransform;
        controller.ProcessCommand(previewMoved);

        EditorWorkspaceViewCommandData previewRestored = previewMoved;
        previewRestored.transformPayload = committedTransform;
        controller.ProcessCommand(previewRestored);

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.objectPayload = object;
        commit.transformPayload = committedTransform;
        controller.ProcessCommand(commit);

        REQUIRE((controller.ViewModel().objects.front().localTransform == committedTransform));
        REQUIRE((!controller.ViewModel().isDirty && !controller.ViewModel().canUndo));
        REQUIRE((controller.CurrentViewportRevision() == ViewportRevision{3}));
    }

    TEST_CASE("Inspector Batch Transform Previews And Commits As One Undoable Change", "[unit][editor]") {
        TestWorkspaceController controller;

        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        controller.ProcessCommand(create);
        REQUIRE((controller.ViewModel().objects.size() == 2));

        const SceneObject first = controller.ViewModel().objects.front();
        const SceneObject second = controller.ViewModel().objects.back();
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform firstEdited = first.localTransform;
        firstEdited.translation = {2.0F, 0.0F, 0.0F};
        Math::Transform secondEdited = second.localTransform;
        secondEdited.translation = {-3.0F, 1.0F, 0.0F};
        const std::vector updates{
            SceneObjectTransformUpdate{first.id, firstEdited},
            SceneObjectTransformUpdate{second.id, secondEdited},
        };

        EditorWorkspaceViewCommandData select;
        select.command = EditorWorkspaceViewCommand::SelectObject;
        select.objectSelection = ObjectSelectionRequest{.objects = {first.id, second.id}, .primary = first.id};
        controller.ProcessCommand(select);
        REQUIRE((controller.ViewModel().selectedObjects == std::vector{first.id, second.id}));

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.transformUpdates = updates;
        controller.ProcessCommand(preview);
        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        for (const SceneObjectTransformUpdate &update : updates) {
            const auto instance = std::ranges::find(controller.ViewportScene().instanceObjects, update.object);
            REQUIRE((instance != controller.ViewportScene().instanceObjects.end()));
            const std::size_t index = static_cast<std::size_t>(std::distance(controller.ViewportScene().instanceObjects.begin(), instance));
            REQUIRE(
                (Math::TransformPoint(controller.ViewportScene().instances[index].localToWorld, {}) == update.localTransform.translation));
        }

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.transformUpdates = updates;
        controller.ProcessCommand(commit);
        REQUIRE((controller.ViewModel().documentRevision.value == initialRevision.value + 1));
        REQUIRE((controller.ViewModel().objects.front().localTransform == firstEdited));
        REQUIRE((controller.ViewModel().objects.back().localTransform == secondEdited));

        EditorWorkspaceViewCommandData undo;
        undo.command = EditorWorkspaceViewCommand::UndoScene;
        controller.ProcessCommand(undo);
        REQUIRE((controller.ViewModel().objects.front().localTransform == first.localTransform));
        REQUIRE((controller.ViewModel().objects.back().localTransform == second.localTransform));
    }

    TEST_CASE("No Op Inspector Batch Commit Clears Preview Without Advancing Document", "[unit][editor]") {
        TestWorkspaceController controller;
        const SceneObject object = controller.ViewModel().objects.front();
        const DocumentRevision initialRevision = controller.ViewModel().documentRevision;
        Math::Transform moved = object.localTransform;
        moved.translation.x += 2.0F;

        EditorWorkspaceViewCommandData preview;
        preview.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
        preview.transformUpdates = std::vector{SceneObjectTransformUpdate{object.id, moved}};
        controller.ProcessCommand(preview);

        EditorWorkspaceViewCommandData commit;
        commit.command = EditorWorkspaceViewCommand::CommitObjectTransform;
        commit.transformUpdates = std::vector{SceneObjectTransformUpdate{object.id, object.localTransform}};
        controller.ProcessCommand(commit);

        REQUIRE((controller.ViewModel().documentRevision == initialRevision));
        REQUIRE((!controller.ViewModel().canUndo));
        REQUIRE((controller.ViewModel().objects.front().localTransform == object.localTransform));
    }
}  // namespace
