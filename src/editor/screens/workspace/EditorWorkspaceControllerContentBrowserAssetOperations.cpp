#include "editor/menu/EditorMenuPlatform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"

namespace Horo::Editor {

    bool EditorWorkspaceController::ResolveContentBrowserAssetTransferMetadata(const std::filesystem::path &projectRoot,
                                                                               const std::filesystem::path &source,
                                                                               Assets::AssetId &assetId,
                                                                               std::vector<std::filesystem::path> &companions) {
        const std::string projectPath = source.lexically_relative(projectRoot).generic_string();
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(projectPath);
        const auto validatedCompanions = ValidatedAssetCompanions(source, true);
        if (record == nullptr || !validatedCompanions.has_value()) {
            m_viewModel.contentBrowserOperationError = record == nullptr ? "workspace.content_browser.operation.asset_required"
                                                                         : "workspace.content_browser.operation.companion_invalid";
            return false;
        }
        assetId = record->id;
        companions = *validatedCompanions;
        return true;
    }

    std::optional<EditorWorkspaceController::ContentBrowserAssetTransferPlan> EditorWorkspaceController::PrepareContentBrowserAssetTransfer(
        const std::filesystem::path &absoluteSource, const std::filesystem::path &absoluteDestinationDirectory, const bool moving) {
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return std::nullopt;
        }
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path source = NormalizeAbsolute(absoluteSource);
        const std::filesystem::path destinationDirectory = NormalizeAbsolute(absoluteDestinationDirectory);
        std::error_code error;
        if (const auto sourceStatus = std::filesystem::symlink_status(source, error);
            error || std::filesystem::is_symlink(sourceStatus) || !std::filesystem::is_regular_file(sourceStatus) ||
            !HasPathPrefix(root, source) || !IsContentBrowserDirectoryTargetAllowed(root, destinationDirectory)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return std::nullopt;
        }
        if (moving && source.parent_path() == destinationDirectory) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.same_folder";
            return std::nullopt;
        }

        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        Assets::AssetId assetId;
        std::vector<std::filesystem::path> companions;
        if (!ResolveContentBrowserAssetTransferMetadata(projectRoot, source, assetId, companions))
            return std::nullopt;

        std::filesystem::path destination = destinationDirectory / source.filename();
        if ((!moving && (destinationDirectory == source.parent_path() || !AssetDestinationAvailable(source, destination, companions))) ||
            (moving && !AssetDestinationAvailable(source, destination, companions))) {
            if (moving) {
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
                return std::nullopt;
            }
            destination = ResolveDuplicateDestination(source, destinationDirectory, companions);
        }
        if (destination.empty()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return std::nullopt;
        }
        std::filesystem::path sourceSidecar = source;
        sourceSidecar += ".horo";
        return ContentBrowserAssetTransferPlan{
            .projectRoot = projectRoot,
            .source = source,
            .sourceSidecar = std::move(sourceSidecar),
            .destination = std::move(destination),
            .companions = std::move(companions),
            .assetId = assetId,
        };
    }

    bool EditorWorkspaceController::CopyContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                              const std::filesystem::path &absoluteDestinationDirectory) {
        const auto plan = PrepareContentBrowserAssetTransfer(absoluteSource, absoluteDestinationDirectory, false);
        if (!plan.has_value())
            return false;

        auto sidecar = ReadSidecarJson(plan->sourceSidecar);
        const Assets::AssetId newId = GenerateRandomAssetId(m_assetRegistry);
        if (!sidecar.has_value() || !newId.IsValid()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.copy_failed";
            return false;
        }
        (*sidecar)["assetId"] = newId.ToString();

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-copy",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return false;
        } else {
            const std::vector<std::byte> sidecarBytes = JsonBytes(*sidecar);
            const std::optional<std::vector<std::filesystem::path>> created =
                CopyContentBrowserCompanions(plan->source, plan->sourceSidecar, plan->destination, plan->companions, sidecarBytes);
            return created.has_value() && PublishCopiedContentBrowserAsset(plan->projectRoot, plan->destination, newId, *created);
        }
    }

    std::optional<std::vector<std::filesystem::path>> EditorWorkspaceController::CopyContentBrowserCompanions(
        const std::filesystem::path &source, const std::filesystem::path &sourceSidecar, const std::filesystem::path &destination,
        const std::vector<std::filesystem::path> &companions, const std::span<const std::byte> sidecarBytes) {
        std::vector<std::filesystem::path> created;
        for (const std::filesystem::path &item : companions) {
            const std::filesystem::path target = CompanionDestination(item, source, destination);
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RemoveCreatedPaths(created);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.name_exists");
                return std::nullopt;
            }
            if (const Result<void> copied =
                    item == sourceSidecar ? m_durableFiles->WriteDurable(target, sidecarBytes) : m_durableFiles->CopyDurable(item, target);
                copied.HasError()) {
                std::error_code cleanupError;
                std::filesystem::remove(target, cleanupError);
                const bool rollbackComplete = RemoveCreatedPaths(created);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.copy_failed");
                return std::nullopt;
            }
            created.emplace_back(target);
        }
        if (m_durableFiles->SyncDirectory(destination.parent_path()).HasError()) {
            const bool rollbackComplete = RemoveCreatedPaths(created);
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.copy_failed");
            return std::nullopt;
        }
        return created;
    }

    bool EditorWorkspaceController::PublishCopiedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                                     const std::filesystem::path &destination,
                                                                     const Assets::AssetId copiedId,
                                                                     const std::vector<std::filesystem::path> &created) {
        const auto rollback = [this, &created, &projectRoot](const std::string_view failureKey) {
            return RollbackContentBrowserMutation(projectRoot, RemoveCreatedPaths(created), failureKey);
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        const std::string destinationProjectPath = destination.lexically_relative(projectRoot).generic_string();
        if (const Assets::AssetRecord *copiedRecord = rebuiltSnapshot.Find(copiedId);
            copiedRecord == nullptr || copiedRecord->sourcePath.String() != destinationProjectPath) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    bool EditorWorkspaceController::RollbackContentBrowserMutation(const std::filesystem::path &projectRoot, const bool rollbackComplete,
                                                                   const std::string_view failureKey) {
        static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit));
        SetContentBrowserRollbackError(rollbackComplete, failureKey);
        return false;
    }

    bool EditorWorkspaceController::MoveContentBrowserAssetTo(const std::filesystem::path &absoluteSource,
                                                              const std::filesystem::path &absoluteDestinationDirectory) {
        const auto plan = PrepareContentBrowserAssetTransfer(absoluteSource, absoluteDestinationDirectory, true);
        if (!plan.has_value())
            return false;

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-move",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return false;
        } else {
            const std::optional<ContentBrowserPathMoves> moved =
                MoveContentBrowserCompanions(plan->source, plan->destination, plan->companions,
                                             "workspace.content_browser.operation.move_failed");
            if (!moved.has_value())
                return false;
            return PublishMovedContentBrowserAsset(plan->projectRoot, plan->destination, plan->assetId, *moved);
        }
    }

    std::optional<EditorWorkspaceController::ContentBrowserPathMoves> EditorWorkspaceController::MoveContentBrowserCompanions(
        const std::filesystem::path &source, const std::filesystem::path &destination, const std::vector<std::filesystem::path> &companions,
        const std::string_view failureKey) {
        ContentBrowserPathMoves moved;
        moved.reserve(companions.size());
        for (const std::filesystem::path &item : companions) {
            const std::filesystem::path target = CompanionDestination(item, source, destination);
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.name_exists");
                return std::nullopt;
            }
            if (const Result<void> renamed = m_durableFiles->AtomicReplace(item, target); renamed.HasError()) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                SetContentBrowserRollbackError(rollbackComplete, failureKey);
                return std::nullopt;
            }
            moved.emplace_back(item, target);
        }
        const bool sourceSynced = m_durableFiles->SyncDirectory(source.parent_path()).HasValue();
        if (const bool destinationSynced =
                source.parent_path() == destination.parent_path() || m_durableFiles->SyncDirectory(destination.parent_path()).HasValue();
            !sourceSynced || !destinationSynced) {
            const bool rollbackComplete = RollbackPathMoves(moved);
            SetContentBrowserRollbackError(rollbackComplete, failureKey);
            return std::nullopt;
        }
        return moved;
    }

    bool EditorWorkspaceController::PublishMovedContentBrowserAsset(const std::filesystem::path &projectRoot,
                                                                    const std::filesystem::path &destination,
                                                                    const Assets::AssetId originalId,
                                                                    const ContentBrowserPathMoves &moved) {
        const auto rollback = [this, &moved, &projectRoot](const std::string_view failureKey) {
            return RollbackContentBrowserMutation(projectRoot, RollbackPathMoves(moved), failureKey);
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        const std::string newProjectPath = destination.lexically_relative(projectRoot).generic_string();
        if (const Assets::AssetRecord *movedRecord = rebuiltSnapshot.Find(originalId);
            movedRecord == nullptr || movedRecord->sourcePath.String() != newProjectPath) {
            return rollback("workspace.content_browser.operation.registry_failed");
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    void EditorWorkspaceController::ClearContentBrowserClipboard() noexcept {
        m_viewModel.contentBrowserClipboard = {};
    }

    void EditorWorkspaceController::SetContentBrowserRollbackError(const bool rollbackComplete, const std::string_view failureKey) {
        m_viewModel.contentBrowserOperationError = rollbackComplete ? failureKey : "workspace.content_browser.operation.rollback_failed";
    }

    void EditorWorkspaceController::ReimportContentBrowserAsset(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr || m_importerCatalog == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }
        const std::filesystem::path target = NormalizeAbsolute(absolutePath);
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-reimport",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            if (auto reimported = Assets::ReimportProjectAsset(
                    Assets::AssetReimportRequest{
                        .absoluteProjectRoot = NormalizeAbsolute(m_viewModel.projectRoot),
                        .absoluteAssetPath = target,
                        .importerCatalog = m_importerCatalog,
                        .registry = m_mutableAssetRegistry,
                        .files = m_durableFiles,
                    },
                    CancellationToken{});
                reimported.HasError()) {
                LOG_ERROR("editor.content_browser", "Reimport failed for %s: %s", target.string().c_str(),
                          reimported.ErrorValue().message.c_str());
                if (reimported.ErrorValue().code.Value() == "asset.import.no_importer")
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_importer_missing";
                else if (reimported.ErrorValue().code.Value() == "asset.registry.source_missing")
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_unavailable";
                else
                    m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reimport_failed";
                return;
            }
            m_assetRegistry = m_mutableAssetRegistry->Snapshot();
            m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
            RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        }
    }

    void EditorWorkspaceController::RevealContentBrowserEntry(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        const std::filesystem::path target = NormalizeAbsolute(absolutePath);
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(target, error);
            error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)) || !HasPathPrefix(root, target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        if (!RevealInNativeFileManager(target)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.reveal_unavailable";
        }
    }

}  // namespace Horo::Editor
