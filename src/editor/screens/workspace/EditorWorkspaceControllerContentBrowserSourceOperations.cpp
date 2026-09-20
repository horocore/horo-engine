#include "editor/menu/EditorMenuPlatform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"

namespace Horo::Editor {

    void EditorWorkspaceController::OpenSourceFile(const SourceOpenRequest &request) {
        m_viewModel.contentBrowserOperationError.clear();
        const Result<SourceOpenResult> opened = m_sourceOpenService.Open(request);
        if (opened.HasError()) {
            const std::string_view code = opened.ErrorValue().code.Value();
            if (code == SourceOpenErrors::Missing.code.Value())
                m_viewModel.contentBrowserOperationError = "workspace.source_open.missing";
            else if (code == SourceOpenErrors::Unsupported.code.Value())
                m_viewModel.contentBrowserOperationError = "workspace.source_open.unsupported";
            else if (code == SourceOpenErrors::EditorUnavailable.code.Value())
                m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
            else
                m_viewModel.contentBrowserOperationError = "workspace.source_open.unsafe";
            return;
        }

        const SourceOpenResult &result = opened.Value();
        bool navigated = false;
        if (m_sourceOpenNavigator) {
            navigated = m_sourceOpenNavigator(result);
        } else {
            navigated = m_diagnosticSourceNavigator(DiagnosticSourceRequest{
                .absolutePath = result.location.absolutePath.string(),
                .line = result.line,
                .column = result.column,
            });
        }
        if (!navigated)
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
    }

    void EditorWorkspaceController::OpenDiagnosticSource(const DiagnosticSourceRequest &source) {
        OpenSourceFile(SourceOpenRequest{
            .path = source.absolutePath,
            .origin = SourceOpenOrigin::DiagnosticNavigation,
            .mode = SourceOpenMode::AllowExternalFallback,
            .line = source.line,
            .column = source.column,
        });
    }

    void EditorWorkspaceController::RenameContentBrowserEntry(const std::filesystem::path &absolutePath, const std::string_view newName) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        const std::optional<ContentBrowserRenamePlan> plan = PrepareContentBrowserRename(absolutePath, newName);
        if (!plan.has_value())
            return;

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-rename",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            const std::optional<ContentBrowserPathMoves> moved =
                MoveContentBrowserCompanions(plan->source, plan->destination, plan->sources,
                                             "workspace.content_browser.operation.rename_failed");
            if (moved.has_value())
                static_cast<void>(PublishRenamedContentBrowserEntries(*moved));
        }
    }

    bool EditorWorkspaceController::ResolveContentBrowserRenameSources(ContentBrowserRenamePlan &plan) {
        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(plan.source.lexically_relative(projectRoot).generic_string());
        const auto companions = ValidatedAssetCompanions(plan.source, record != nullptr);
        if (!companions.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.companion_invalid";
            return false;
        }
        plan.sources = *companions;
        for (const std::filesystem::path &item : plan.sources) {
            const std::filesystem::path target = CompanionDestination(item, plan.source, plan.destination);
            if (DirectoryContainsPortableName(target.parent_path(), target.filename().string(), item)) {
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
                return false;
            }
        }
        return true;
    }

    std::optional<EditorWorkspaceController::ContentBrowserRenamePlan> EditorWorkspaceController::PrepareContentBrowserRename(
        const std::filesystem::path &absolutePath, const std::string_view newName) {
        if (!absolutePath.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return std::nullopt;
        }
        ContentBrowserRenamePlan plan{.source = NormalizeAbsolute(absolutePath)};
        std::filesystem::path requestedName{newName};
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, plan.source) || requestedName != requestedName.filename() ||
            !IsPortableEntryName(newName)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return std::nullopt;
        }

        std::error_code error;
        const bool regularFile = std::filesystem::is_regular_file(plan.source, error);
        if (regularFile && requestedName.extension().empty())
            requestedName += plan.source.extension().string();
        if (regularFile && requestedName.extension() != plan.source.extension()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return std::nullopt;
        }
        plan.destination = plan.source.parent_path() / requestedName;
        if (plan.destination == plan.source)
            return std::nullopt;
        if (DirectoryContainsPortableName(plan.source.parent_path(), requestedName.string(), plan.source)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return std::nullopt;
        }

        const bool directory = std::filesystem::is_directory(plan.source, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return std::nullopt;
        }
        if (directory) {
            plan.sources.push_back(plan.source);
            return plan;
        }

        if (!ResolveContentBrowserRenameSources(plan))
            return std::nullopt;
        return plan;
    }

    bool EditorWorkspaceController::PublishRenamedContentBrowserEntries(const ContentBrowserPathMoves &moved) {
        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const auto rollback = [this, &moved, &projectRoot]() {
            const bool rollbackComplete = RollbackPathMoves(moved);
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.registry_failed");
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            return rollback();
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        if (const bool registryPreserved = rebuiltSnapshot.Records().size() == m_assetRegistry.Records().size() &&
                                           std::ranges::all_of(m_assetRegistry.Records(),
                                                               [&rebuiltSnapshot](const Assets::AssetRecord &record) {
            return rebuiltSnapshot.Find(record.id) != nullptr;
        });
            !registryPreserved) {
            return rollback();
        }
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }

    void EditorWorkspaceController::DeleteContentBrowserEntry(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr || m_mutableAssetRegistry == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        if (!std::filesystem::path{absolutePath}.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        const std::filesystem::path source = NormalizeAbsolute(absolutePath);
        if (!IsDirectContentBrowserEntry(m_viewModel.contentBrowser, source)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-delete",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            const std::optional<ContentBrowserDeletePlan> plan = PrepareContentBrowserDelete(source);
            if (!plan.has_value())
                return;
            const std::optional<std::filesystem::path> trashDirectory = CreateContentBrowserTrash(*plan);
            if (!trashDirectory.has_value())
                return;
            const std::optional<ContentBrowserPathMoves> moved = MoveContentBrowserEntriesToTrash(*plan, *trashDirectory);
            if (moved.has_value())
                static_cast<void>(PublishContentBrowserDeletion(*plan, *trashDirectory, *moved));
        }
    }

    std::optional<EditorWorkspaceController::ContentBrowserDeletePlan> EditorWorkspaceController::PrepareContentBrowserDelete(
        const std::filesystem::path &source) {
        std::error_code error;
        const bool directory = std::filesystem::is_directory(source, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        ContentBrowserDeletePlan plan{.projectRoot = NormalizeAbsolute(m_viewModel.projectRoot), .source = source};
        if (directory) {
            plan.sources.push_back(source);
            return plan;
        }

        plan.deletedAssetProjectPath = source.lexically_relative(plan.projectRoot).generic_string();
        const Assets::AssetRecord *record = m_assetRegistry.FindByPath(plan.deletedAssetProjectPath);
        if (record != nullptr)
            plan.deletedAssetId = record->id;
        const auto companions = ValidatedAssetCompanions(source, record != nullptr);
        if (!companions.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.companion_invalid";
            return std::nullopt;
        }
        plan.sources = *companions;
        return plan;
    }

    std::optional<std::filesystem::path> EditorWorkspaceController::CreateContentBrowserTrash(const ContentBrowserDeletePlan &plan) {
        std::error_code error;
        const auto stamp =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::filesystem::path trashRoot = plan.projectRoot / ".horo" / "local" / "trash";
        std::filesystem::create_directories(trashRoot, error);
        if (error) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        const std::optional<std::filesystem::path> trashDirectory = CreateUniqueTrashDirectory(trashRoot, stamp);
        if (!trashDirectory.has_value()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }

        nlohmann::json manifest{{"schemaVersion", 1},
                                {"originalAbsolutePath", plan.source.string()},
                                {"deletedAtUnixMicroseconds", stamp},
                                {"entries", nlohmann::json::array()}};
        for (const std::filesystem::path &item : plan.sources) {
            manifest["entries"].push_back({{"originalAbsolutePath", item.string()}, {"trashFileName", item.filename().string()}});
        }
        if (const std::optional<std::filesystem::path> manifestPath = SelectTrashManifestPath(*trashDirectory, plan.sources);
            !manifestPath.has_value() || m_durableFiles->WriteDurable(*manifestPath, JsonBytes(manifest)).HasError()) {
            std::filesystem::remove_all(*trashDirectory, error);
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.delete_failed";
            return std::nullopt;
        }
        return trashDirectory;
    }

    std::optional<EditorWorkspaceController::ContentBrowserPathMoves> EditorWorkspaceController::MoveContentBrowserEntriesToTrash(
        const ContentBrowserDeletePlan &plan, const std::filesystem::path &trashDirectory) {
        std::error_code error;
        ContentBrowserPathMoves moved;
        moved.reserve(plan.sources.size());
        for (const std::filesystem::path &item : plan.sources) {
            const std::filesystem::path target = trashDirectory / item.filename();
            if (!PathDoesNotExist(target)) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                if (rollbackComplete)
                    std::filesystem::remove_all(trashDirectory, error);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
                return std::nullopt;
            }
            std::filesystem::rename(item, target, error);
            if (error) {
                const bool rollbackComplete = RollbackPathMoves(moved);
                if (rollbackComplete)
                    std::filesystem::remove_all(trashDirectory, error);
                SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
                return std::nullopt;
            }
            moved.emplace_back(item, target);
        }
        if (m_durableFiles->SyncDirectory(plan.source.parent_path()).HasError() ||
            m_durableFiles->SyncDirectory(trashDirectory).HasError()) {
            const bool rollbackComplete = RollbackPathMoves(moved);
            if (rollbackComplete)
                std::filesystem::remove_all(trashDirectory, error);
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.delete_failed");
            return std::nullopt;
        }
        return moved;
    }

    bool EditorWorkspaceController::PublishContentBrowserDeletion(const ContentBrowserDeletePlan &plan,
                                                                  const std::filesystem::path &trashDirectory,
                                                                  const ContentBrowserPathMoves &moved) {
        const auto rollback = [this, &moved, &trashDirectory, &plan]() {
            const bool rollbackComplete = RollbackPathMoves(moved);
            if (rollbackComplete) {
                std::error_code error;
                std::filesystem::remove_all(trashDirectory, error);
            }
            static_cast<void>(Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, plan.projectRoot, Assets::AssetRegistryOpenMode::Edit));
            SetContentBrowserRollbackError(rollbackComplete, "workspace.content_browser.operation.registry_failed");
            return false;
        };
        if (auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, plan.projectRoot, Assets::AssetRegistryOpenMode::Edit);
            rebuilt.HasError() || rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Failed) {
            return rollback();
        }
        const Assets::AssetRegistrySnapshot rebuiltSnapshot = m_mutableAssetRegistry->Snapshot();
        if (plan.deletedAssetId.has_value() && (rebuiltSnapshot.Find(*plan.deletedAssetId) != nullptr ||
                                                rebuiltSnapshot.FindByPath(plan.deletedAssetProjectPath) != nullptr)) {
            return rollback();
        }
        LOG_INFO("editor.content_browser", "Moved asset entry to recoverable project trash: %s", trashDirectory.string().c_str());
        m_assetRegistry = rebuiltSnapshot;
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(plan.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        return true;
    }
}  // namespace Horo::Editor
