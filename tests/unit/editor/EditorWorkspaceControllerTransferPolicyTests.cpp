#include "EditorWorkspaceControllerPolicyTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace HoroEditorWorkspaceControllerPolicyTests;

    template <typename TFileSystem> class ContentBrowserTransferFixture final {
    public:
        explicit ContentBrowserTransferFixture(const std::string_view suffix)
            : projectRoot_(std::filesystem::temp_directory_path() /
                           ("horo-workspace-transfer-" + std::string(suffix) + "-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
              sourceDirectory_(projectRoot_ / "assets/Source"), targetDirectory_(projectRoot_ / "assets/Target"),
              copyFailureDirectory_(projectRoot_ / "assets/CopyFailure"), replaceFailureDirectory_(projectRoot_ / "assets/ReplaceFailure"),
              source_(sourceDirectory_ / "crate.horoasset"), mutations_(files) {
            CreateAssetFiles();
            REQUIRE((Assets::RebuildAssetRegistry(registry_, projectRoot_, Assets::AssetRegistryOpenMode::Edit).HasValue()));
            REQUIRE((runtimeScene_.Startup(cancellation_.Token()).HasValue()));
            controller_ = std::make_unique<EditorWorkspaceController>(projectRoot_, runtimeScene_, registry_.Snapshot(),
                                                                      EditorWorkspaceDependencies{.mutableAssetRegistry = &registry_,
                                                                                                  .mutations = &mutations_,
                                                                                                  .durableFiles = &files});
        }

        ~ContentBrowserTransferFixture() {
            std::error_code cleanupError;
            std::filesystem::remove_all(projectRoot_, cleanupError);
        }

        [[nodiscard]] EditorWorkspaceController &Controller() noexcept {
            return *controller_;
        }

        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept {
            return projectRoot_;
        }

        [[nodiscard]] const std::filesystem::path &SourceDirectory() const noexcept {
            return sourceDirectory_;
        }

        [[nodiscard]] const std::filesystem::path &TargetDirectory() const noexcept {
            return targetDirectory_;
        }

        [[nodiscard]] const std::filesystem::path &CopyFailureDirectory() const noexcept {
            return copyFailureDirectory_;
        }

        [[nodiscard]] const std::filesystem::path &ReplaceFailureDirectory() const noexcept {
            return replaceFailureDirectory_;
        }

        [[nodiscard]] const std::filesystem::path &Source() const noexcept {
            return source_;
        }

        [[nodiscard]] TFileSystem &FileSystem() noexcept {
            return files;
        }

        [[nodiscard]] ProjectMutationCoordinator &Mutations() noexcept {
            return mutations_;
        }

    private:
        void CreateAssetFiles() {
            std::filesystem::create_directories(sourceDirectory_);
            std::filesystem::create_directories(targetDirectory_);
            std::filesystem::create_directories(copyFailureDirectory_);
            std::filesystem::create_directories(replaceFailureDirectory_);
            std::filesystem::create_directories(projectRoot_ / ".horo/local");
            std::ofstream payload(source_, std::ios::binary);
            payload << "asset";
            std::ofstream metadata(source_.string() + ".horo", std::ios::binary);
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "20112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }

        std::filesystem::path projectRoot_;
        std::filesystem::path sourceDirectory_;
        std::filesystem::path targetDirectory_;
        std::filesystem::path copyFailureDirectory_;
        std::filesystem::path replaceFailureDirectory_;
        std::filesystem::path source_;
        TFileSystem files;
        ProjectMutationCoordinator mutations_;
        Runtime::RuntimeSceneService runtimeScene_;
        CancellationSource cancellation_;
        Assets::AssetRegistry registry_;
        std::unique_ptr<EditorWorkspaceController> controller_;
    };

    class SourceProjectFixture final {
    public:
        SourceProjectFixture()
            : projectRoot_(std::filesystem::temp_directory_path() /
                           ("horo-workspace-source-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
              sourcePath_(projectRoot_ / "source/gameplay/Player.cpp"), unsupportedPath_(projectRoot_ / "assets/model.bin") {
            std::filesystem::create_directories(sourcePath_.parent_path());
            std::filesystem::create_directories(unsupportedPath_.parent_path());
            std::ofstream source(sourcePath_, std::ios::binary);
            source << "int Player() { return 0; }\n";
            std::ofstream unsupported(unsupportedPath_, std::ios::binary);
            unsupported << "binary";
        }

        ~SourceProjectFixture() {
            std::error_code cleanupError;
            std::filesystem::remove_all(projectRoot_, cleanupError);
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return projectRoot_;
        }

        [[nodiscard]] const std::filesystem::path &Source() const noexcept {
            return sourcePath_;
        }

        [[nodiscard]] const std::filesystem::path &Unsupported() const noexcept {
            return unsupportedPath_;
        }

    private:
        std::filesystem::path projectRoot_;
        std::filesystem::path sourcePath_;
        std::filesystem::path unsupportedPath_;
    };

    TEST_CASE("Content browser transfer rejects an orphan asset", "[unit][editor][content-browser]") {
        ContentBrowserTransferFixture<NativeDurableFileSystem> fixture{"validation-orphan"};
        const std::filesystem::path orphan = fixture.SourceDirectory() / "orphan.horoasset";
        {
            std::ofstream payload(orphan, std::ios::binary);
            payload << "asset";
            std::ofstream metadata(orphan.string() + ".horo", std::ios::binary);
            metadata << R"({
  "schemaVersion": 1,
  "assetId": "10112233-4455-6677-8899-aabbccddeeff",
  "assetType": "core.mesh"
})";
        }
        EditorWorkspaceViewCommandData transfer;
        transfer.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        transfer.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = orphan.string(),
            .absoluteDestinationDirectory = fixture.TargetDirectory().string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        fixture.Controller().ProcessCommand(transfer);
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.asset_required"));
    }

    TEST_CASE("Content browser transfer rejects same-folder and busy requests", "[unit][editor][content-browser]") {
        ContentBrowserTransferFixture<NativeDurableFileSystem> fixture{"validation-state"};
        EditorWorkspaceViewCommandData sameFolder;
        sameFolder.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        sameFolder.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = fixture.Source().string(),
            .absoluteDestinationDirectory = fixture.SourceDirectory().string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        fixture.Controller().ProcessCommand(sameFolder);
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.same_folder"));

        auto held = fixture.Mutations().TryAcquire(ProjectMutationRequest{
            .projectRoot = fixture.ProjectRoot(),
            .owner = ProjectMutationOwner::Asset,
            .operationId = "test-held-transfer",
        });
        REQUIRE((held.HasValue()));
        EditorWorkspaceViewCommandData busy;
        busy.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        busy.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = fixture.Source().string(),
            .absoluteDestinationDirectory = fixture.TargetDirectory().string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        fixture.Controller().ProcessCommand(busy);
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.busy"));
    }

    TEST_CASE("Content browser copy rolls back a directory sync failure", "[unit][editor][content-browser]") {
        ContentBrowserTransferFixture<SyncFailingFilesystem> fixture{"rollback-copy"};
        fixture.FileSystem().failSync = true;
        EditorWorkspaceViewCommandData copy;
        copy.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        copy.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = fixture.Source().string(),
            .absoluteDestinationDirectory = fixture.CopyFailureDirectory().string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        fixture.Controller().ProcessCommand(copy);
        REQUIRE((std::filesystem::is_regular_file(fixture.Source())));
        REQUIRE_FALSE(std::filesystem::exists(fixture.CopyFailureDirectory() / fixture.Source().filename()));
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.copy_failed"));
    }

    TEST_CASE("Content browser copy rejects an invalid sidecar", "[unit][editor][content-browser]") {
        ContentBrowserTransferFixture<SyncFailingFilesystem> fixture{"rollback-sidecar"};
        {
            std::ofstream invalidSidecar(fixture.Source().string() + ".horo", std::ios::binary | std::ios::trunc);
            invalidSidecar << "not-json";
        }
        EditorWorkspaceViewCommandData copy;
        copy.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        copy.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = fixture.Source().string(),
            .absoluteDestinationDirectory = fixture.ReplaceFailureDirectory().string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        fixture.Controller().ProcessCommand(copy);
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.copy_failed"));
    }

    TEST_CASE("Content browser move rolls back an atomic replace failure", "[unit][editor][content-browser]") {
        ContentBrowserTransferFixture<SyncFailingFilesystem> fixture{"rollback-replace"};
        fixture.FileSystem().failReplace = true;
        EditorWorkspaceViewCommandData move;
        move.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        move.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = fixture.Source().string(),
            .absoluteDestinationDirectory = fixture.ReplaceFailureDirectory().string(),
            .mode = ContentBrowserTransferMode::Move,
        };
        fixture.Controller().ProcessCommand(move);
        REQUIRE((std::filesystem::is_regular_file(fixture.Source())));
        REQUIRE_FALSE(std::filesystem::exists(fixture.ReplaceFailureDirectory() / fixture.Source().filename()));
        REQUIRE((fixture.Controller().ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.move_failed"));
    }

    TEST_CASE("Workspace source commands preserve validated routing failures", "[unit][editor][source]") {
        SourceProjectFixture project;
        bool sourceNavigatorCalled = false;
        std::uint32_t navigatedLine = 0;
        FocusedWorkspaceController controller{project.Root(), {}, [&](const SourceOpenResult &result) {
            sourceNavigatorCalled = true;
            navigatedLine = result.line;
            return true;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{
            .path = project.Source(),
            .origin = SourceOpenOrigin::Command,
            .mode = SourceOpenMode::EmbeddedOnly,
            .line = 12,
        };
        controller.ProcessCommand(command);
        REQUIRE(sourceNavigatorCalled);
        REQUIRE((navigatedLine == 12));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());

        command.sourceOpenRequest = SourceOpenRequest{
            .path = project.Root() / "source/gameplay/Missing.cpp",
            .origin = SourceOpenOrigin::Command,
            .mode = SourceOpenMode::EmbeddedOnly,
        };
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.missing"));

        command.sourceOpenRequest = SourceOpenRequest{
            .path = project.Unsupported(),
            .origin = SourceOpenOrigin::Command,
            .mode = SourceOpenMode::EmbeddedOnly,
        };
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unsupported"));

        command.sourceOpenRequest = SourceOpenRequest{
            .path = project.Root().parent_path() / "outside.cpp",
            .origin = SourceOpenOrigin::Command,
            .mode = SourceOpenMode::EmbeddedOnly,
        };
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unsafe"));
    }

    TEST_CASE("Workspace source commands report an unavailable navigator", "[unit][editor][source]") {
        SourceProjectFixture project;
        std::vector<DocumentOpenDisposition> dispositions;
        FocusedWorkspaceController controller{project.Root(), {}, [&dispositions](const SourceOpenResult &result) {
            if (result.document.has_value())
                dispositions.push_back(result.document->disposition);
            return false;
        }};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{
            .path = project.Source(),
            .origin = SourceOpenOrigin::Command,
            .mode = SourceOpenMode::EmbeddedOnly,
        };
        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unavailable"));
        REQUIRE((dispositions == std::vector<DocumentOpenDisposition>{DocumentOpenDisposition::Opened}));

        controller.ProcessCommand(command);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unavailable"));
        REQUIRE((dispositions == std::vector<DocumentOpenDisposition>{DocumentOpenDisposition::Opened, DocumentOpenDisposition::Opened}));
    }

    TEST_CASE("Workspace content browser commands report unavailable and unsafe targets", "[unit][editor][content-browser]") {
        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-workspace-content-browser-errors-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        FocusedWorkspaceController controller{projectRoot};

        EditorWorkspaceViewCommandData transfer;
        transfer.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        transfer.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = (projectRoot / "assets/source.horoasset").string(),
            .absoluteDestinationDirectory = (projectRoot / "assets/target").string(),
            .mode = ContentBrowserTransferMode::Copy,
        };
        controller.ProcessCommand(transfer);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.unavailable"));

        EditorWorkspaceViewCommandData rename;
        rename.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry;
        rename.stringPayload = (projectRoot / "assets/source.horoasset").string();
        rename.secondaryStringPayload = "renamed.horoasset";
        controller.ProcessCommand(rename);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.unavailable"));

        EditorWorkspaceViewCommandData remove;
        remove.command = EditorWorkspaceViewCommand::DeleteContentBrowserEntry;
        remove.stringPayload = (projectRoot / "assets/source.horoasset").string();
        controller.ProcessCommand(remove);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.unavailable"));

        EditorWorkspaceViewCommandData reimport;
        reimport.command = EditorWorkspaceViewCommand::ReimportContentBrowserAsset;
        reimport.stringPayload = (projectRoot / "assets/source.horoasset").string();
        controller.ProcessCommand(reimport);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.unavailable"));

        EditorWorkspaceViewCommandData reveal;
        reveal.command = EditorWorkspaceViewCommand::RevealContentBrowserEntry;
        reveal.stringPayload = (projectRoot / "assets/missing.horoasset").string();
        controller.ProcessCommand(reveal);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.invalid_target"));

        EditorWorkspaceViewCommandData createNative;
        createNative.command = EditorWorkspaceViewCommand::CreateNativeBehavior;
        createNative.gameplayBehaviorRequest = CreateGameplayBehaviorRequest{
            .destination = (projectRoot / "assets/scripts").string(),
            .baseName = "UnavailableBehavior",
            .kind = GameplayBehaviorKind::Native,
        };
        controller.ProcessCommand(createNative);
        REQUIRE((controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.unavailable"));
    }

    TEST_CASE("Workspace layout commands ignore incomplete panel payloads", "[unit][editor][layout]") {
        FocusedWorkspaceController controller{"test-project"};

        EditorWorkspaceViewCommandData activePanel;
        activePanel.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        controller.ProcessCommand(activePanel);
        activePanel.targetIndex = 99;
        activePanel.stringPayload = "horo.invalid";
        controller.ProcessCommand(activePanel);
        activePanel.targetIndex = 3;
        activePanel.stringPayload = "horo.source";
        controller.ProcessCommand(activePanel);

        EditorWorkspaceViewCommandData splitTop;
        splitTop.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        splitTop.targetIndex = 0;
        splitTop.stringPayload = "horo.inspector";
        splitTop.sideDockSlot = SideDockSlot::Top;
        controller.ProcessCommand(splitTop);

        EditorWorkspaceViewCommandData reorder;
        reorder.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
        controller.ProcessCommand(reorder);
        reorder.stringPayload = "horo.missing";
        reorder.activityBarSlot = ActivityBarSlot{ActivityBarRail::Left, 0, 0};
        controller.ProcessCommand(reorder);
        reorder.stringPayload = "horo.viewport";
        reorder.activityBarSlot = ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0};
        controller.ProcessCommand(reorder);

        EditorWorkspaceViewCommandData dock;
        dock.command = EditorWorkspaceViewCommand::DockWorkspacePanel;
        controller.ProcessCommand(dock);
        dock.stringPayload = "horo.inspector";
        controller.ProcessCommand(dock);
        dock.workspaceDropTarget = WorkspacePanelDropTarget{"workspace.document", WorkspacePanelHost::DropKind::TabCenter};
        controller.ProcessCommand(dock);
    }

    TEST_CASE("Workspace resize commands ignore incomplete payloads", "[unit][editor][layout]") {
        FocusedWorkspaceController controller{"test-project"};
        EditorWorkspaceViewCommandData resize;
        resize.command = EditorWorkspaceViewCommand::ResizePanel;
        controller.ProcessCommand(resize);
        resize.targetIndex = 99;
        resize.floatPayload = 222.0F;
        controller.ProcessCommand(resize);
        resize.targetIndex = 0;
        resize.layoutPayload = WorkspaceLayoutSize{};
        controller.ProcessCommand(resize);
        REQUIRE((controller.ViewModel().leftPanelWidth == 222.0F));
        resize.targetIndex = 1;
        resize.floatPayload = 333.0F;
        controller.ProcessCommand(resize);
        REQUIRE((controller.ViewModel().rightPanelWidth == 333.0F));
        resize.targetIndex = 2;
        resize.floatPayload = 444.0F;
        controller.ProcessCommand(resize);
        REQUIRE((controller.ViewModel().bottomPanelHeight == 444.0F));
    }
}  // namespace
