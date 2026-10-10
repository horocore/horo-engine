#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "EditorWorkspaceControllerPolicyTestSupport.h"
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
    using TestWorkspaceController = HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;
    using HoroEditorWorkspaceControllerPolicyTests::WriteTestMeshPayload;

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

    /** @brief Verify Broken Asset Drop. */
    void VerifyBrokenAssetDrop(TestWorkspaceController &controller, const Result<Assets::AssetId> &brokenId,
                               const Result<Assets::AssetTypeId> &assetType, const std::vector<NotificationEvent> &notifications) {
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
    }

    /** @brief Verify Asset Drop History. */
    void VerifyAssetDropHistory(TestWorkspaceController &controller, const Result<Assets::AssetId> &assetId,
                                const Result<Assets::AssetTypeId> &assetType, const std::vector<NotificationEvent> &notifications) {
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
    }

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

        VerifyBrokenAssetDrop(controller, brokenId, assetType, notifications);

        VerifyAssetDropHistory(controller, assetId, assetType, notifications);

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

}  // namespace
