#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace HoroEditorWorkspaceControllerPolicyTests;

    TEST_CASE("Gameplay behavior creation command writes native source and schedules a build", "[unit][editor][behavior]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-create-native-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path requestedDirectory = projectRoot / "assets/scripts";
        std::filesystem::create_directories(requestedDirectory);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Runtime::RuntimeSceneService runtimeScene;
        CancellationSource cancellation;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CreateNativeBehavior;
        command.gameplayBehaviorRequest = CreateGameplayBehaviorRequest{
            .destination = requestedDirectory.string(),
            .baseName = "PlayerBehavior",
            .kind = GameplayBehaviorKind::Native,
        };
        controller.ProcessCommand(command);

        const std::filesystem::path sourceDirectory = projectRoot / "source/gameplay";
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));
        REQUIRE((std::filesystem::is_regular_file(sourceDirectory / "PlayerBehavior.cpp")));
        REQUIRE((std::filesystem::is_regular_file(sourceDirectory / "GameModule.cpp")));
        controller.UpdateGameplaySources(0.5F);
        REQUIRE((controller.ViewModel().contentBrowserOperationError.empty()));

        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists"));
        auto held = mutations.TryAcquire(ProjectMutationRequest{
            .projectRoot = projectRoot,
            .owner = ProjectMutationOwner::Asset,
            .operationId = "test-held-lease",
        });
        REQUIRE((held.HasValue()));
        command.gameplayBehaviorRequest = CreateGameplayBehaviorRequest{.destination = requestedDirectory.string(),
                                                                        .baseName = "SecondBehavior",
                                                                        .kind = GameplayBehaviorKind::Native};
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.busy"));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace discard scene recovery refuses a held mutation", "[unit][editor][persistence][recovery]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-held-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
        }
        {
            EditorWorkspaceController reopened{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE(reopened.ViewModel().recoveryAvailable);
            reopened.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::RestoreSceneRecovery});
            REQUIRE(std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery"));
            auto held = mutations.TryAcquire(ProjectMutationRequest{.projectRoot = projectRoot,
                                                                    .owner = ProjectMutationOwner::Asset,
                                                                    .operationId = "test-held-recovery-discard"});
            REQUIRE((held.HasValue()));
            reopened.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::DiscardSceneRecovery});
            REQUIRE(std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery"));
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    class AssetDropPolicyFixture final {
    public:
        AssetDropPolicyFixture()
            : projectRoot_(
                  std::filesystem::temp_directory_path() /
                  ("horo-workspace-asset-drop-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            const std::filesystem::path meshesDirectory = projectRoot_ / "assets/Meshes";
            std::filesystem::create_directories(meshesDirectory);
            WriteTestMeshPayload(meshesDirectory / "chair.horoasset");
            const auto assetId = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
            const auto assetType = Assets::AssetTypeId::Parse("core.mesh");
            const auto sourcePath = ProjectPath::Parse("assets/Meshes/chair.horoasset");
            const auto metadataPath = ProjectPath::Parse("assets/Meshes/chair.horoasset.horo");
            const auto unsupportedId = Assets::AssetId::Parse("22112233-4455-6677-8899-aabbccddeeff");
            const auto unsupportedType = Assets::AssetTypeId::Parse("core.texture");
            const auto unsupportedSourcePath = ProjectPath::Parse("assets/Meshes/unsupported.horoasset");
            const auto unsupportedMetadataPath = ProjectPath::Parse("assets/Meshes/unsupported.horoasset.horo");
            REQUIRE((assetId.HasValue() && assetType.HasValue() && sourcePath.HasValue() && metadataPath.HasValue() &&
                     unsupportedId.HasValue() && unsupportedType.HasValue() && unsupportedSourcePath.HasValue() &&
                     unsupportedMetadataPath.HasValue()));
            REQUIRE((registry_
                         .Publish({Assets::AssetRecord{assetId.Value(), assetType.Value(), sourcePath.Value(), metadataPath.Value()},
                                   Assets::AssetRecord{unsupportedId.Value(), unsupportedType.Value(), unsupportedSourcePath.Value(),
                                                       unsupportedMetadataPath.Value()}})
                         .status == Assets::AssetRegistryBuildStatus::Complete));
            controller_ = std::make_unique<FocusedWorkspaceController>(projectRoot_);
            controller_->RefreshAssets(registry_.Snapshot());
        }

        ~AssetDropPolicyFixture() {
            std::error_code cleanupError;
            std::filesystem::remove_all(projectRoot_, cleanupError);
        }

        [[nodiscard]] FocusedWorkspaceController &Controller() noexcept {
            return *controller_;
        }

    private:
        std::filesystem::path projectRoot_;
        Assets::AssetRegistry registry_;
        std::unique_ptr<FocusedWorkspaceController> controller_;
    };

    TEST_CASE("Asset scene drops reject invalid primitive and stale payloads", "[unit][editor][asset-drop]") {
        AssetDropPolicyFixture fixture;
        auto &controller = fixture.Controller();
        const std::size_t initialObjectCount = controller.ViewModel().objects.size();
        controller.ProcessCommand(EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::CreatePrimitive,
                                                                 .primitivePayload = Runtime::PrimitiveId{"primitive.does-not-exist"}});
        REQUIRE((controller.ViewModel().objects.size() == initialObjectCount));
        EditorWorkspaceViewCommandData staleDrop;
        staleDrop.command = EditorWorkspaceViewCommand::InstantiateAsset;
        staleDrop.assetSceneDrop = AssetSceneDropRequest{.assetId = "not-an-asset-id",
                                                         .assetType = "core.mesh",
                                                         .target = AssetSceneDropTarget::HierarchyRoot,
                                                         .documentRevision = controller.ViewModel().documentRevision};
        controller.ProcessCommand(staleDrop);
        REQUIRE((controller.ViewModel().objects.size() == initialObjectCount));
    }

    TEST_CASE("Asset scene drops reject mismatched and unsupported types", "[unit][editor][asset-drop]") {
        AssetDropPolicyFixture fixture;
        auto &controller = fixture.Controller();
        const std::size_t initialObjectCount = controller.ViewModel().objects.size();
        for (const AssetSceneDropRequest request : {AssetSceneDropRequest{.assetId = "00112233-4455-6677-8899-aabbccddeeff",
                                                                          .assetType = "core.texture",
                                                                          .target = AssetSceneDropTarget::HierarchyRoot,
                                                                          .documentRevision = controller.ViewModel().documentRevision},
                                                    AssetSceneDropRequest{.assetId = "22112233-4455-6677-8899-aabbccddeeff",
                                                                          .assetType = "core.texture",
                                                                          .target = AssetSceneDropTarget::HierarchyRoot,
                                                                          .documentRevision = controller.ViewModel().documentRevision}}) {
            controller.ProcessCommand(
                EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::InstantiateAsset, .assetSceneDrop = request});
            REQUIRE((controller.ViewModel().objects.size() == initialObjectCount));
        }
    }

    TEST_CASE("Asset scene drops reject missing parents and invalid placement", "[unit][editor][asset-drop]") {
        AssetDropPolicyFixture fixture;
        auto &controller = fixture.Controller();
        const std::size_t initialObjectCount = controller.ViewModel().objects.size();
        controller.ProcessCommand(
            EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::InstantiateAsset,
                                           .assetSceneDrop =
                                               AssetSceneDropRequest{.assetId = "00112233-4455-6677-8899-aabbccddeeff",
                                                                     .assetType = "core.mesh",
                                                                     .parent = SceneObjectId{999999},
                                                                     .target = AssetSceneDropTarget::HierarchyRoot,
                                                                     .documentRevision = controller.ViewModel().documentRevision}});
        REQUIRE((controller.ViewModel().objects.size() == initialObjectCount));
        controller.ProcessCommand(
            EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::InstantiateAsset,
                                           .assetSceneDrop =
                                               AssetSceneDropRequest{.assetId = "00112233-4455-6677-8899-aabbccddeeff",
                                                                     .assetType = "core.mesh",
                                                                     .target = AssetSceneDropTarget::Viewport,
                                                                     .normalizedX = 2.0F,
                                                                     .normalizedY = 0.5F,
                                                                     .aspect = 1.0F,
                                                                     .documentRevision = controller.ViewModel().documentRevision}});
        REQUIRE((controller.ViewModel().objects.size() == initialObjectCount));
    }
}  // namespace
