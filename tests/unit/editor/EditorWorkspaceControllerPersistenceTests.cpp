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

    /** @brief Verify Saved Identity Reload. */
    void VerifySavedIdentityReload(EditorWorkspaceController &controller, const std::filesystem::path &saveAsPath) {
        {
            std::ofstream external(saveAsPath, std::ios::binary | std::ios::trunc);
            external << "{\"schemaVersion\":1,\"objects\":[]}\n";
        }
        auto reload = SceneCommand(EditorWorkspaceViewCommand::ReloadExternalScene);
        controller.ProcessCommand(reload);
        REQUIRE((controller.ViewModel().objects.empty()));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(saveAsPath)));
    }

    /** @brief Verify External Scene Comparison. */
    void VerifyExternalSceneComparison(EditorWorkspaceController &controller) {
        auto captured = controller.CaptureExternalSceneComparison();
        REQUIRE((captured.HasValue()));
        auto comparison = LoadSceneDocumentComparison(std::move(captured).Value());
        REQUIRE((comparison.HasValue()));
        REQUIRE((comparison.Value().removedFromDisk == 1));
        REQUIRE((comparison.Value().addedOnDisk == 0));
        REQUIRE((comparison.Value().modified == 0));
        REQUIRE((std::filesystem::path{comparison.Value().absoluteScenePath}.is_absolute()));
    }

    TEST_CASE("Workspace Save Persists And Reopens The Default Scene", "[unit][editor][persistence]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        PrepareEmptyDefaultScene(projectRoot, scenePath);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));

        {
            EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE((controller.ViewModel().objects.empty()));
            REQUIRE((!controller.ViewModel().isDirty));

            auto create = BoxCreationCommand();
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

            auto save = SceneCommand(EditorWorkspaceViewCommand::SaveScene);
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
        PrepareEmptyDefaultScene(projectRoot, scenePath);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        auto create = BoxCreationCommand();
        controller.ProcessCommand(create);
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().has_value()));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(scenePath)));

        auto saveCopy = SceneCommand(EditorWorkspaceViewCommand::SaveSceneCopyAs);
        saveCopy.stringPayload = copyPath.string();
        controller.ProcessCommand(saveCopy);
        REQUIRE((std::filesystem::exists(copyPath)));
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(scenePath)));

        auto saveAs = SceneCommand(EditorWorkspaceViewCommand::SaveSceneAs);
        saveAs.stringPayload = saveAsPath.string();
        controller.ProcessCommand(saveAs);
        REQUIRE((std::filesystem::exists(saveAsPath)));
        REQUIRE((!controller.ViewModel().isDirty));
        REQUIRE((controller.CurrentScenePath().value() == std::filesystem::weakly_canonical(saveAsPath)));

        controller.ProcessCommand(create);
        auto save = SceneCommand(EditorWorkspaceViewCommand::SaveScene);
        controller.ProcessCommand(save);
        REQUIRE((!controller.ViewModel().isDirty));

        auto renamedFingerprint = InspectProjectSceneFingerprint(projectRoot, saveAsPath);
        auto copyFingerprint = InspectProjectSceneFingerprint(projectRoot, copyPath);
        REQUIRE((renamedFingerprint.HasValue()));
        REQUIRE((copyFingerprint.HasValue()));
        REQUIRE((renamedFingerprint.Value() != copyFingerprint.Value()));

        VerifySavedIdentityReload(controller, saveAsPath);

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Workspace Classifies And Explicitly Restores Scene Recovery", "[unit][editor][persistence][recovery]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-scene-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path scenePath = projectRoot / "assets/scenes/main.horo";
        PrepareEmptyDefaultScene(projectRoot, scenePath);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));

        {
            EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            auto create = BoxCreationCommand();
            controller.ProcessCommand(create);
            controller.UpdateAutosave(60.0F, 1);
            REQUIRE((std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));
        }

        {
            EditorWorkspaceController reopened{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};
            REQUIRE((reopened.ViewModel().recoveryAvailable));
            REQUIRE((reopened.ViewModel().objects.empty()));
            REQUIRE((!reopened.ViewModel().isDirty));

            auto restore = SceneCommand(EditorWorkspaceViewCommand::RestoreSceneRecovery);
            reopened.ProcessCommand(restore);
            REQUIRE((!reopened.ViewModel().recoveryAvailable));
            REQUIRE((reopened.ViewModel().objects.size() == 1));
            REQUIRE((reopened.ViewModel().isDirty));
            REQUIRE((std::filesystem::exists(projectRoot / ".horo/local/recovery/default-scene.hororecovery")));

            auto save = SceneCommand(EditorWorkspaceViewCommand::SaveScene);
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
        PrepareEmptyDefaultScene(projectRoot, scenePath);

        SyncFailingFilesystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        auto create = BoxCreationCommand();
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
        PrepareEmptyDefaultScene(projectRoot, scenePath);

        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtimeScene;
        REQUIRE((runtimeScene.Startup(cancellation.Token()).HasValue()));
        EditorWorkspaceController controller{projectRoot, runtimeScene, {}, {.mutations = &mutations, .durableFiles = &files}};

        auto create = BoxCreationCommand();
        controller.ProcessCommand(create);
        controller.UpdateAutosave(60.0F, 1);
        {
            std::ofstream external(scenePath, std::ios::binary | std::ios::trunc);
            external << "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
        }

        auto save = SceneCommand(EditorWorkspaceViewCommand::SaveScene);
        controller.ProcessCommand(save);
        REQUIRE((controller.ViewModel().isDirty));
        REQUIRE((controller.ViewModel().sceneExternalConflict));
        VerifyExternalSceneComparison(controller);

        auto reload = SceneCommand(EditorWorkspaceViewCommand::ReloadExternalScene);
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

        auto overwrite = SceneCommand(EditorWorkspaceViewCommand::OverwriteExternalScene);
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
        PrepareEmptyDefaultScene(projectRoot, scenePath);

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

            auto reload = SceneCommand(EditorWorkspaceViewCommand::ReloadExternalScene);
            controller.ProcessCommand(reload);
            REQUIRE((!controller.ViewModel().sceneExternalConflict));
            REQUIRE((!controller.ViewModel().isDirty));
        }
        jobs.Shutdown(ShutdownPolicy::Drain);

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

}  // namespace
