#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;

    /** @brief Uses a genuine assetless Play Scene and its canonical workspace
     * command routes; no worker/native completion is faked. */
    class ReplacementFixture final {
    public:
        ReplacementFixture()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-play-replacement-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
              scene(root / "assets/scenes/main.horo"), mutations(files) {
            PrepareEmptyDefaultScene(root, scene);
            Tests::WriteReplicationProject(root);
            SeedDefaultSceneField();
            const auto configured = SetProjectDefaultScenePath(root, scene, mutations, files);
            INFO((configured.HasError() ? configured.ErrorValue().message : "Canonical project default Scene configured."));
            REQUIRE(configured.HasValue());
            REQUIRE(Application::LoadProjectSourceDocument(root).HasValue());
            const auto loaded = LoadProjectDefaultScene(root);
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value());
            REQUIRE(loaded.Value()->objects.empty());
            REQUIRE(preview.Startup(cancellation.Token()).HasValue());
            controller =
                std::make_unique<EditorWorkspaceController>(root, preview, Assets::AssetRegistrySnapshot{},
                                                            EditorWorkspaceDependencies{.mutations = &mutations, .durableFiles = &files});
            INFO((controller->InitializationError() ? controller->InitializationError()->message : "Canonical Scene admitted."));
            REQUIRE_FALSE(controller->InitializationError());
            auto camera = SceneCommand(EditorWorkspaceViewCommand::CreatePrimitive);
            camera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
            controller->ProcessCommand(camera);
            REQUIRE(controller->ViewModel().objects.size() == 1);
            Send(EditorWorkspaceViewCommand::StartPlay);
            for (unsigned phase = 0; phase < 8 && controller->ViewModel().playState == EditorPlayState::Starting; ++phase)
                controller->UpdatePlayPresentation(0.0F);
            INFO(controller->ViewModel().playError);
            REQUIRE(controller->ViewModel().playState == EditorPlayState::Playing);
        }

        /** @brief Preserves the complete current project while explicitly admitting an initially unselected default Scene. */
        void SeedDefaultSceneField() const {
            const auto path = root / ".horo/project.json";
            std::ifstream input(path, std::ios::binary);
            REQUIRE(input.good());
            auto metadata = nlohmann::json::parse(input);
            metadata.at("settings")["defaultScene"] = "";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << metadata.dump(2);
            REQUIRE(output.good());
        }

        ~ReplacementFixture() {
            controller.reset();
            preview.Shutdown();
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        /** @brief Supplies explicit user commands through the actual controller
         * dispatcher. */
        void Send(EditorWorkspaceViewCommand command) {
            controller->ProcessCommand(SceneCommand(command));
        }

        /** @brief Replaces only fixture-owned disk content, leaving the active
         * authoring document unchanged. */
        void EmptyDiskScene() const {
            std::ofstream output(scene, std::ios::binary | std::ios::trunc);
            output << "{\"schemaVersion\":1,\"objects\":[]}\n";
            REQUIRE(output.good());
        }

        std::filesystem::path root, scene;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations;
        CancellationSource cancellation;
        Runtime::RuntimeSceneService preview;
        std::unique_ptr<EditorWorkspaceController> controller;
    };
}  // namespace

TEST_CASE("Play external replacement waits for Stop and rejects authoring "
          "edits after consent",
          "[editor][play][save][recovery]") {
    ReplacementFixture f;
    f.EmptyDiskScene();
    f.Send(EditorWorkspaceViewCommand::ReloadExternalScene);
    REQUIRE(f.controller->ViewModel().objects.size() == 1);
    SECTION("unchanged consent resumes only on a later owner update") {
        f.controller->UpdatePlayPresentation(0.0F);
        CHECK(f.controller->ViewModel().playState == EditorPlayState::Idle);
        CHECK(f.controller->ViewModel().objects.empty());
        CHECK_FALSE(f.controller->ViewModel().isDirty);
    }
    SECTION("intervening edit invalidates the pending replacement") {
        f.controller->ProcessCommand(BoxCreationCommand());
        REQUIRE(f.controller->ViewModel().objects.size() == 2);
        f.controller->UpdatePlayPresentation(0.0F);
        CHECK(f.controller->ViewModel().objects.size() == 2);
        CHECK(f.controller->ViewModel().isDirty);
        f.Send(EditorWorkspaceViewCommand::ReloadExternalScene);
        CHECK(f.controller->ViewModel().objects.empty());
    }
}

TEST_CASE("Play recovery consent expires after an authoring edit and requires "
          "a fresh request",
          "[editor][play][save][recovery]") {
    ReplacementFixture f;
    f.controller->UpdateAutosave(60.0F, 1);
    const auto recovery =
        InspectProjectSceneRecovery(std::filesystem::weakly_canonical(f.root), std::filesystem::weakly_canonical(f.scene));
    REQUIRE(recovery.HasValue());
    REQUIRE(recovery.Value());
    REQUIRE(recovery.Value()->objects.size() == 1);
    f.Send(EditorWorkspaceViewCommand::RestoreSceneRecovery);
    REQUIRE(f.controller->ViewModel().objects.size() == 1);
    f.controller->ProcessCommand(BoxCreationCommand());
    REQUIRE(f.controller->ViewModel().objects.size() == 2);
    f.controller->UpdatePlayPresentation(0.0F);
    CHECK(f.controller->ViewModel().objects.size() == 2);
    f.Send(EditorWorkspaceViewCommand::RestoreSceneRecovery);
    CHECK(f.controller->ViewModel().objects.size() == 1);
    CHECK(f.controller->ViewModel().isDirty);
}
