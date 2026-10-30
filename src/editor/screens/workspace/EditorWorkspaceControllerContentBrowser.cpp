#include "Horo/Assets/AssetReimport.h"
#include "Horo/Editor/ProjectIntegrityValidatorService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/menu/EditorMenuPlatform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"
#include "editor/screens/workspace/prefab/PrefabAssetPreview.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor {

    void EditorWorkspaceController::RebuildContentBrowserProjection(const std::filesystem::path &projectRoot,
                                                                    const std::filesystem::path &requestedDirectory) {
        RefreshPhysicsMaterials();
        m_viewModel.contentBrowser = BuildContentBrowserDirectory(projectRoot, requestedDirectory, m_assetRegistry, m_importerCatalog);
        ScheduleContentBrowserPreviews();
    }

    void EditorWorkspaceController::ScheduleContentBrowserPreviews() {
        for (const PendingContentBrowserPreview &pending : m_pendingContentBrowserPreviews)
            if (pending.handle.has_value())
                static_cast<void>(pending.handle->RequestCancel());
        m_pendingContentBrowserPreviews.clear();
        for (auto &entry : m_viewModel.contentBrowser.entries) {
            entry.previewImage = {};
            entry.previewState = ContentBrowserPreviewState::Unrequested;
            entry.previewError.reset();
        }
        ++m_contentBrowserPreviewGeneration;
        m_contentBrowserNextPreviewEntry = 0;
        AdmitContentBrowserPreviews();
    }

    /** @copydoc EditorWorkspaceController::AdmitContentBrowserPreviews */
    void EditorWorkspaceController::AdmitContentBrowserPreviews() {
        constexpr std::size_t MaximumPending = 32;
        if (m_assetPreviews == nullptr)
            return;
        while (m_pendingContentBrowserPreviews.size() < MaximumPending &&
               m_contentBrowserNextPreviewEntry < m_viewModel.contentBrowser.entries.size()) {
            auto &entry = m_viewModel.contentBrowser.entries[m_contentBrowserNextPreviewEntry++];
            std::optional<Assets::AssetPreviewRequest> request;
            if (entry.IsPrefab()) {
                // Project source capture takes an exclusive cooperative lease; serialize prefab work within this owner.
                if (std::ranges::any_of(m_pendingContentBrowserPreviews, [](const auto &pending) {
                    return pending.request.assetType.Value() == "core.prefab";
                })) {
                    --m_contentBrowserNextPreviewEntry;
                    break;
                }
                auto asset = Assets::AssetId::Parse(entry.assetId);
                if (asset.HasError())
                    continue;
                request = Assets::AssetPreviewRequest{
                    .contributionId = "core.prefab.thumbnail",
                    .moduleId = "horo.editor",
                    .moduleVersion = std::to_string(m_assetRegistry.Revision().value),
                    // A fresh directory publication invalidates nested-source cache entries even when identity metadata is unchanged.
                    .providerVersion = std::to_string(m_contentBrowserPreviewGeneration),
                    .absoluteAssetPath = entry.absolutePath,
                    .assetType = *entry.registeredType,
                    .provider = CreatePrefabAssetPreviewProvider(m_assetRegistry, asset.Value(), m_prefabSourceCapture),
                };
            } else if (m_importerCatalog != nullptr)
                request = BuildPreviewRequest(entry, *m_importerCatalog);
            if (!request)
                continue;
            auto submitted = m_assetPreviews->Submit(*request);
            if (submitted.HasError() && submitted.ErrorValue().code.Value() != "asset.preview.queue_full") {
                entry.previewState = ContentBrowserPreviewState::Unavailable;
                entry.previewError = submitted.ErrorValue();
                continue;
            }
            entry.previewState = ContentBrowserPreviewState::Queued;
            m_pendingContentBrowserPreviews.push_back(PendingContentBrowserPreview{
                .absolutePath = entry.absolutePath,
                .generation = m_contentBrowserPreviewGeneration,
                .request = std::move(*request),
                .handle = submitted.HasValue() ? std::optional{std::move(submitted).Value()} : std::nullopt,
            });
        }
    }

    void EditorWorkspaceController::PollContentBrowserPreviews() {
        std::erase_if(m_pendingContentBrowserPreviews, [this](PendingContentBrowserPreview &pending) {
            return PollContentBrowserPreview(pending);
        });
        AdmitContentBrowserPreviews();
    }

    bool EditorWorkspaceController::PollContentBrowserPreview(PendingContentBrowserPreview &pending) {
        if (!pending.handle.has_value()) {
            auto submitted = m_assetPreviews->Submit(pending.request);
            if (submitted.HasError()) {
                if (submitted.ErrorValue().code.Value() == "asset.preview.queue_full")
                    return false;
                if (const auto entry =
                        std::ranges::find(m_viewModel.contentBrowser.entries, pending.absolutePath, &ContentBrowserEntry::absolutePath);
                    entry != m_viewModel.contentBrowser.entries.end() && pending.generation == m_contentBrowserPreviewGeneration) {
                    entry->previewState = ContentBrowserPreviewState::Unavailable;
                    entry->previewError = submitted.ErrorValue();
                }
                return true;
            }
            pending.handle = std::move(submitted).Value();
        }
        if (const Assets::AssetPreviewState state = pending.handle->State();
            state == Assets::AssetPreviewState::Queued || state == Assets::AssetPreviewState::Running)
            return false;
        const auto entry = std::ranges::find(m_viewModel.contentBrowser.entries, pending.absolutePath, &ContentBrowserEntry::absolutePath);
        if (pending.generation != m_contentBrowserPreviewGeneration || entry == m_viewModel.contentBrowser.entries.end())
            return true;
        if (auto completed = pending.handle->TakeResult(); completed.HasValue()) {
            entry->previewImage = std::move(completed).Value().image;
            entry->previewState = ContentBrowserPreviewState::Ready;
            entry->previewError.reset();
        } else {
            entry->previewError = completed.ErrorValue();
            if (completed.ErrorValue().code.Value() == "project.migration.locked") {
                pending.handle.reset();
                return false;
            }
            entry->previewState = ContentBrowserPreviewState::Unavailable;
        }
        return true;
    }

    void EditorWorkspaceController::RefreshAssets(const Assets::AssetRegistrySnapshot &assetRegistry) {
        if (assetRegistry.Revision() == m_viewModel.assetRegistryRevision)
            return;
        CancelAssetPlacementPreview();
        m_prefabSources.reset();
        m_assetRegistry = assetRegistry;
        m_viewModel.assetRegistryRevision = assetRegistry.Revision();
        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        ReconcileContentBrowserNavigation();
        RefreshPrefabHierarchy(m_document.Snapshot());
        for (const auto &entry : m_prefabDocuments) {
            if (const auto inspected = entry->InspectExternal(); inspected.HasError())
                m_viewModel.prefabDocumentError = inspected.ErrorValue();
            entry->ReleaseStage();
        }
        m_prefabStageAttemptRevision = 0;
        RefreshPrefabDocuments();
    }

    void EditorWorkspaceController::UpdateContentBrowser() {
        PollContentBrowserPreviews();
        if (!m_contentBrowserRefreshPending)
            return;
        if (!m_contentBrowserLoadingPresented) {
            m_contentBrowserLoadingPresented = true;
            return;
        }

        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        RefreshContentBrowserAfterMutation();
    }

    void EditorWorkspaceController::RefreshContentBrowserAfterMutation() {
        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        if (m_mutableAssetRegistry != nullptr) {
            if (auto rebuilt = RebuildProjectAssetRegistry(m_viewModel.projectRoot);
                rebuilt.HasError() || rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Failed) {
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.registry_failed";
                RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
                ReconcileContentBrowserNavigation();
                return;
            }
            m_assetRegistry = m_mutableAssetRegistry->Snapshot();
            m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        }
        RebuildContentBrowserProjection(m_viewModel.projectRoot, m_viewModel.contentBrowser.absoluteCurrentPath);
        ReconcileContentBrowserNavigation();
    }

    void EditorWorkspaceController::RequestContentBrowserRefresh() {
        if (m_contentBrowserRefreshPending)
            return;
        m_contentBrowserRefreshPending = true;
        m_contentBrowserLoadingPresented = false;
        m_viewModel.contentBrowser.loadState = ContentBrowserLoadState::Loading;
        m_viewModel.contentBrowserOperationError.clear();
    }

    void EditorWorkspaceController::ReconcileContentBrowserNavigation() {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        const auto isValidHistoryEntry = [&root, &current](const std::filesystem::path &entry) {
            const std::filesystem::path normalized = NormalizeAbsolute(entry);
            return !normalized.empty() && normalized != current && IsProjectContentBrowserDirectoryTargetAllowed(root, normalized);
        };
        std::erase_if(m_contentBrowserBackHistory, [&isValidHistoryEntry](const std::filesystem::path &entry) {
            return !isValidHistoryEntry(entry);
        });
        std::erase_if(m_contentBrowserForwardHistory, [&isValidHistoryEntry](const std::filesystem::path &entry) {
            return !isValidHistoryEntry(entry);
        });
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowser(const std::filesystem::path &absoluteDirectory, const bool recordHistory) {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::filesystem::path destination = NormalizeAbsolute(absoluteDirectory);
        if (!IsProjectContentBrowserDirectoryTargetAllowed(root, destination)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        if (destination == current)
            return;

        m_contentBrowserRefreshPending = false;
        m_contentBrowserLoadingPresented = false;
        if (recordHistory && !current.empty()) {
            m_contentBrowserBackHistory.push_back(current);
            m_contentBrowserForwardHistory.clear();
        }
        RebuildContentBrowserProjection(m_viewModel.projectRoot, destination);
        m_viewModel.contentBrowserOperationError.clear();
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserBack() {
        if (m_contentBrowserBackHistory.empty())
            return;
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        while (!m_contentBrowserBackHistory.empty()) {
            const std::filesystem::path destination = m_contentBrowserBackHistory.back();
            m_contentBrowserBackHistory.pop_back();
            if (!IsProjectContentBrowserDirectoryTargetAllowed(m_viewModel.projectRoot, destination)) {
                continue;
            }
            if (!current.empty())
                m_contentBrowserForwardHistory.push_back(current);
            NavigateContentBrowser(destination, false);
            break;
        }
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserForward() {
        if (m_contentBrowserForwardHistory.empty())
            return;
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        while (!m_contentBrowserForwardHistory.empty()) {
            const std::filesystem::path destination = m_contentBrowserForwardHistory.back();
            m_contentBrowserForwardHistory.pop_back();
            if (!IsProjectContentBrowserDirectoryTargetAllowed(m_viewModel.projectRoot, destination)) {
                continue;
            }
            if (!current.empty())
                m_contentBrowserBackHistory.push_back(current);
            NavigateContentBrowser(destination, false);
            break;
        }
        m_viewModel.contentBrowserCanNavigateBack = !m_contentBrowserBackHistory.empty();
        m_viewModel.contentBrowserCanNavigateForward = !m_contentBrowserForwardHistory.empty();
    }

    void EditorWorkspaceController::NavigateContentBrowserUp() {
        const std::filesystem::path root = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteRootPath);
        const std::filesystem::path current = NormalizeAbsolute(m_viewModel.contentBrowser.absoluteCurrentPath);
        if (!root.empty() && current != root)
            NavigateContentBrowser(current.parent_path(), true);
    }

    void EditorWorkspaceController::DuplicateContentBrowserAsset(const std::filesystem::path &absolutePath) {
        m_viewModel.contentBrowserOperationError.clear();
        static_cast<void>(CopyContentBrowserAssetTo(NormalizeAbsolute(absolutePath), NormalizeAbsolute(absolutePath).parent_path()));
    }

    void EditorWorkspaceController::SetContentBrowserClipboard(const std::filesystem::path &absolutePath,
                                                               const ContentBrowserClipboardMode mode) {
        m_viewModel.contentBrowserOperationError.clear();
        const std::filesystem::path source = NormalizeAbsolute(absolutePath);
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(source, error);
            !IsDirectContentBrowserEntry(m_viewModel.contentBrowser, source) || error || !std::filesystem::is_regular_file(status)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        m_viewModel.contentBrowserClipboard = {
            .mode = mode,
            .absoluteSourcePath = source.string(),
        };
    }

    void EditorWorkspaceController::PasteContentBrowserAsset(const std::filesystem::path &absoluteDirectory) {
        using enum ContentBrowserClipboardMode;
        m_viewModel.contentBrowserOperationError.clear();
        const ContentBrowserClipboardState clipboard = m_viewModel.contentBrowserClipboard;
        if (clipboard.mode == None || clipboard.absoluteSourcePath.empty()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.clipboard_empty";
            return;
        }

        const std::filesystem::path source = NormalizeAbsolute(clipboard.absoluteSourcePath);
        const std::filesystem::path destination = NormalizeAbsolute(absoluteDirectory);
        bool succeeded{};
        if (clipboard.mode == Copy)
            succeeded = CopyContentBrowserAssetTo(source, destination);
        else
            succeeded = MoveContentBrowserAssetTo(source, destination);
        if (succeeded && clipboard.mode == Move)
            ClearContentBrowserClipboard();
    }

    void EditorWorkspaceController::TransferContentBrowserAsset(const ContentBrowserAssetTransferRequest &request) {
        m_viewModel.contentBrowserOperationError.clear();
        if (!std::filesystem::path{request.absoluteSourcePath}.is_absolute() ||
            !std::filesystem::path{request.absoluteDestinationDirectory}.is_absolute()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }
        const std::filesystem::path source = NormalizeAbsolute(request.absoluteSourcePath);
        const std::filesystem::path destination = NormalizeAbsolute(request.absoluteDestinationDirectory);
        if (request.mode == ContentBrowserTransferMode::Copy)
            static_cast<void>(CopyContentBrowserAssetTo(source, destination));
        else
            static_cast<void>(MoveContentBrowserAssetTo(source, destination));
    }

    void EditorWorkspaceController::CreateContentBrowserFolder(const std::filesystem::path &absoluteDirectory,
                                                               const std::string_view name) {
        LOG_INFO("editor.asset_actions", "Create folder requested: directory='%s' name='%s'", absoluteDirectory.string().c_str(),
                 std::string{name}.c_str());
        m_viewModel.contentBrowserOperationError.clear();
        if (m_mutations == nullptr || m_durableFiles == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }
        const std::filesystem::path directory = NormalizeAbsolute(absoluteDirectory);
        const std::filesystem::path requestedName{name};
        if (!IsContentBrowserDirectoryTargetAllowed(m_viewModel.contentBrowser.absoluteRootPath, directory) ||
            requestedName != requestedName.filename() || !IsPortableEntryName(name)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return;
        }
        if (DirectoryContainsPortableName(directory, name)) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return;
        }

        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = m_viewModel.projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = "content-browser-create-folder",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            if (std::error_code error; !std::filesystem::create_directory(directory / requestedName, error) || error) {
                LOG_ERROR("editor.asset_actions", "Create folder failed: path='%s' error='%s'",
                          (directory / requestedName).string().c_str(), error.message().c_str());
                m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_folder_failed";
                return;
            }
            static_cast<void>(m_durableFiles->SyncDirectory(directory));
            LOG_INFO("editor.asset_actions", "Create folder completed: path='%s'", (directory / requestedName).string().c_str());
            RefreshContentBrowserAfterMutation();
        }
    }

}  // namespace Horo::Editor
