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

    /** @brief Verify Trashed Asset Pair. */
    void VerifyTrashedAssetPair(const std::filesystem::path &projectRoot, const std::filesystem::path &renamed) {
        const std::filesystem::path trashRoot = projectRoot / ".horo/local/trash";
        REQUIRE((std::filesystem::is_directory(trashRoot)));
        const std::filesystem::path trashEntry = std::filesystem::directory_iterator(trashRoot)->path();
        REQUIRE((std::filesystem::is_regular_file(trashEntry / renamed.filename())));
        REQUIRE((std::filesystem::is_regular_file(trashEntry / (renamed.filename().string() + ".horo"))));
        std::ifstream manifestStream(trashEntry / "trash.json");
        const nlohmann::json manifest = nlohmann::json::parse(manifestStream);
        const std::filesystem::path recordedOriginal = manifest.at("originalAbsolutePath").get<std::string>();
        REQUIRE((recordedOriginal == std::filesystem::weakly_canonical(renamed)));
    }

    TEST_CASE("Content Browser Rename And Delete Preserve Registry Consistency", "[unit][editor]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-asset-mutation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path meshesDirectory = projectRoot / "assets/Meshes";
        std::filesystem::create_directories(meshesDirectory);
        std::filesystem::create_directories(projectRoot / ".horo/local");
        const std::filesystem::path source = meshesDirectory / "crate.horoasset";
        WriteIdentityAsset(source, "00112233-4455-6677-8899-aabbccddeeff");

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
        VerifyTrashedAssetPair(projectRoot, renamed);

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
        WriteIdentityAsset(source, "40112233-4455-6677-8899-aabbccddeeff");

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
        WriteCorruptAsset(corrupt);

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
        WriteIdentityAsset(trackedAsset, "50112233-4455-6677-8899-aabbccddeeff");
        const std::filesystem::path corruptAsset = assetDirectory / "corrupt.horoasset";
        WriteCorruptAsset(corruptAsset);
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

}  // namespace
