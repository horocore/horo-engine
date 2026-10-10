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

    /** @brief Identity evidence retained across duplicate/copy/move action phases. */
    struct DuplicatedAsset final {
        Assets::AssetId originalId;
        Assets::AssetId duplicateId;
        std::filesystem::path duplicatePath;
    };

    /** @brief Verify Name Collision And Unicode Rename. */
    void VerifyNameCollisionAndUnicodeRename(EditorWorkspaceController &controller, const std::filesystem::path &sourceDirectory,
                                             const std::filesystem::path &targetDirectory, std::filesystem::path &source) {
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
    }

    /** @brief Verify Reserved Folders And Escaping Target. */
    void VerifyReservedFoldersAndEscapingTarget(EditorWorkspaceController &controller, const std::filesystem::path &sourceDirectory,
                                                const std::filesystem::path &outsideDirectory, const std::filesystem::path &projectRoot,
                                                EditorWorkspaceViewCommandData &copy) {
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
    }

    /** @brief Verify Duplicated Asset Identity. */
    [[nodiscard]] DuplicatedAsset VerifyDuplicatedAssetIdentity(EditorWorkspaceController &controller, Assets::AssetRegistry &registry,
                                                                const std::filesystem::path &source,
                                                                const std::filesystem::path &sourceDirectory) {
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

        return {originalId, duplicateId, duplicatePath};
    }

    /** @brief Verify Copied Asset Identity. */
    [[nodiscard]] EditorWorkspaceViewCommandData VerifyCopiedAssetIdentity(EditorWorkspaceController &controller,
                                                                           Assets::AssetRegistry &registry,
                                                                           const std::filesystem::path &source,
                                                                           const std::filesystem::path &targetDirectory,
                                                                           const Assets::AssetId &originalId) {
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

        return paste;
    }

    /** @brief Verify Clipboard Move Identity. */
    [[nodiscard]] std::filesystem::path VerifyClipboardMoveIdentity(EditorWorkspaceController &controller, Assets::AssetRegistry &registry,
                                                                    const std::filesystem::path &source,
                                                                    const std::filesystem::path &targetDirectory,
                                                                    const std::filesystem::path &duplicatePath,
                                                                    const Assets::AssetId &duplicateId,
                                                                    EditorWorkspaceViewCommandData &paste) {
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

        return movedPath;
    }

    /** @brief Verify Drag Transfer Identity. */
    void VerifyDragTransferIdentity(EditorWorkspaceController &controller, Assets::AssetRegistry &registry,
                                    const std::filesystem::path &source, const std::filesystem::path &movedPath,
                                    const std::filesystem::path &dragCopyDirectory, const std::filesystem::path &dragMoveDirectory,
                                    const Assets::AssetId &originalId, const Assets::AssetId &duplicateId) {
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
    }

    /** @brief Verify Transfer Navigation And Folder Creation. */
    void VerifyTransferNavigationAndFolderCreation(EditorWorkspaceController &controller, const std::filesystem::path &dragCopyDirectory,
                                                   const std::filesystem::path &sourceDirectory,
                                                   const std::filesystem::path &targetDirectory, EditorWorkspaceViewCommandData &navigate) {
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
        WriteIdentityAsset(source, "00112233-4455-6677-8899-aabbccddeeff");

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

        const auto duplicated = VerifyDuplicatedAssetIdentity(controller, registry, source, sourceDirectory);
        const auto &originalId = duplicated.originalId;
        const auto &duplicateId = duplicated.duplicateId;
        const auto &duplicatePath = duplicated.duplicatePath;

        auto paste = VerifyCopiedAssetIdentity(controller, registry, source, targetDirectory, originalId);

        const auto movedPath =
            VerifyClipboardMoveIdentity(controller, registry, source, targetDirectory, duplicatePath, duplicateId, paste);

        const std::filesystem::path dragCopyDirectory = projectRoot / "assets/DragCopy";
        const std::filesystem::path dragMoveDirectory = projectRoot / "assets/DragMove";
        VerifyDragTransferIdentity(controller, registry, source, movedPath, dragCopyDirectory, dragMoveDirectory, originalId, duplicateId);

        VerifyTransferNavigationAndFolderCreation(controller, dragCopyDirectory, sourceDirectory, targetDirectory, navigate);

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
        WriteIdentityAsset(source, "20112233-4455-6677-8899-aabbccddeeff");

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
        WriteIdentityAsset(source, "30112233-4455-6677-8899-aabbccddeeff");

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

        VerifyNameCollisionAndUnicodeRename(controller, sourceDirectory, targetDirectory, source);

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

        VerifyReservedFoldersAndEscapingTarget(controller, sourceDirectory, outsideDirectory, projectRoot, copy);

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
        std::filesystem::remove_all(outsideDirectory, cleanupError);
    }

}  // namespace
