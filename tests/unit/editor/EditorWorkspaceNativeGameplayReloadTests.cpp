#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModule.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    class TemporaryGameplayProject final {
    public:
        TemporaryGameplayProject()
            : root_(std::filesystem::temp_directory_path() /
                    ("horo-workspace-native-reload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
              artifact_(HORO_TEST_GAME_MODULE_PATH),
              descriptorRevision_(Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)) {
            std::filesystem::create_directories(root_ / "assets");
            Publish(Gameplay::CurrentGameplayBuildFingerprint());
        }

        ~TemporaryGameplayProject() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return root_;
        }

        void Publish(const std::string_view fingerprint) {
            manifest_ = Tests::WriteGameplayModuleManifest(root_, artifact_, "game.tests", fingerprint, descriptorRevision_);
        }

        void SignalManifestChange() const {
            Tests::AdvanceLastWriteTime(manifest_);
        }

    private:
        std::filesystem::path root_;
        std::filesystem::path artifact_;
        std::uint64_t descriptorRevision_{};
        std::filesystem::path manifest_;
    };

    class NativeReloadWorkspace final {
    public:
        NativeReloadWorkspace() {
            REQUIRE(runtimeScene_.Startup(cancellation_.Token()).HasValue());
            CommitRuntimePreview();
            EditorWorkspaceViewCommandData createCamera;
            createCamera.command = EditorWorkspaceViewCommand::CreatePrimitive;
            createCamera.primitivePayload = Runtime::PrimitiveId{"primitive.object.camera"};
            controller_.ProcessCommand(createCamera);
            CommitRuntimePreview();
            StartPlay();
        }

        void BlockRollbackStorage() const {
            std::ofstream blocker{RollbackDirectory(), std::ios::binary};
            REQUIRE(blocker.good());
            blocker << "not a directory";
            REQUIRE(blocker.good());
        }

        void UnblockRollbackStorage() const {
            REQUIRE(std::filesystem::remove(RollbackDirectory()));
        }

        void Publish(const std::string_view fingerprint) {
            project_.Publish(fingerprint);
        }

        void ReloadAtSafePoint() {
            project_.SignalManifestChange();
            controller_.UpdateGameplaySources(0.5F);
            controller_.UpdatePlayFixed({}, 1.0 / 60.0);
        }

        void StartPlay() {
            Send(EditorWorkspaceViewCommand::StartPlay);
        }

        void StopPlay() {
            Send(EditorWorkspaceViewCommand::StopPlay);
        }

        void PausePlay() {
            Send(EditorWorkspaceViewCommand::PausePlay);
        }

        void ResumePlay() {
            Send(EditorWorkspaceViewCommand::ResumePlay);
        }

        void StepPlay() {
            Send(EditorWorkspaceViewCommand::StepPlay);
        }

        void UpdatePlayPresentation(const float elapsedSeconds) {
            controller_.UpdatePlayPresentation(elapsedSeconds);
        }

        void UpdatePlayFixed(const double fixedDeltaSeconds) {
            controller_.UpdatePlayFixed({}, fixedDeltaSeconds);
        }

        void CreateNativeGameplaySource() const {
            const std::filesystem::path sourceDirectory = project_.Root() / "source" / "gameplay";
            std::filesystem::create_directories(sourceDirectory);
            std::ofstream source{sourceDirectory / "ExistingBehavior.cpp", std::ios::binary | std::ios::trunc};
            REQUIRE(source.good());
            source << "int ExistingBehavior() { return 0; }\n";
            REQUIRE(source.good());
        }

        void RequirePlaying() const {
            REQUIRE(controller_.ViewModel().playState == EditorPlayState::Playing);
            REQUIRE(controller_.ViewModel().playError.empty());
        }

        void RequireIdle() const {
            REQUIRE(controller_.ViewModel().playState == EditorPlayState::Idle);
        }

        void RequirePaused() const {
            REQUIRE(controller_.ViewModel().playState == EditorPlayState::Paused);
        }

        void RequireFailed() const {
            REQUIRE(controller_.ViewModel().playState == EditorPlayState::Failed);
            REQUIRE_FALSE(controller_.ViewModel().playError.empty());
        }

    private:
        /** @brief Commits the queued authoring preview before play clones the active runtime scene. */
        void CommitRuntimePreview() {
            const Runtime::FrameContext context{1, {}, 0.0, 0, {}, false, cancellation_.Token()};
            REQUIRE(runtimeScene_.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
            controller_.SynchronizeRuntimeScenePreview();
        }

        [[nodiscard]] std::filesystem::path RollbackDirectory() const {
            return project_.Root() / ".horo" / "local" / "gameplay_module_rollback";
        }

        void Send(const EditorWorkspaceViewCommand command) {
            EditorWorkspaceViewCommandData request;
            request.command = command;
            controller_.ProcessCommand(request);
        }

        TemporaryGameplayProject project_;
        NativeDurableFileSystem files_;
        ProjectMutationCoordinator mutations_{files_};
        Runtime::RuntimeSceneService runtimeScene_;
        CancellationSource cancellation_;
        EditorWorkspaceController controller_{project_.Root(), runtimeScene_, {}, {.mutations = &mutations_, .durableFiles = &files_}};
    };
}  // namespace

TEST_CASE("Native gameplay reload commits compatible generations and rolls back rejected candidates", "[unit][editor][gameplay]") {
    NativeReloadWorkspace workspace;
    workspace.RequirePlaying();

    workspace.BlockRollbackStorage();
    workspace.ReloadAtSafePoint();
    workspace.RequirePlaying();

    workspace.UnblockRollbackStorage();
    workspace.ReloadAtSafePoint();
    workspace.RequirePlaying();

    workspace.Publish("incompatible-test-fingerprint");
    workspace.ReloadAtSafePoint();
    workspace.RequirePlaying();

    workspace.Publish(Gameplay::CurrentGameplayBuildFingerprint());
    workspace.StopPlay();
    workspace.RequireIdle();
    workspace.StartPlay();
    workspace.RequirePlaying();
    workspace.StopPlay();
    workspace.RequireIdle();
}

TEST_CASE("Workspace play controls pause, step, resume, and reject unavailable native builds", "[unit][editor][gameplay]") {
    NativeReloadWorkspace workspace;
    workspace.RequirePlaying();

    workspace.PausePlay();
    workspace.RequirePaused();
    workspace.StepPlay();
    workspace.RequirePaused();
    workspace.ResumePlay();
    workspace.RequirePlaying();
    workspace.UpdatePlayPresentation(-1.0F);
    workspace.UpdatePlayFixed(0.0);

    workspace.StopPlay();
    workspace.RequireIdle();
    workspace.CreateNativeGameplaySource();
    workspace.StartPlay();
    workspace.RequireFailed();
}
