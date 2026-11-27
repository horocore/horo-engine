#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::PrepareNavigationSceneReplacement */
    bool EditorWorkspaceController::PrepareNavigationSceneReplacement(const char *operation) {
        const auto closed = ShutdownNavigationDocuments();
        if (closed.HasValue())
            return true;
        LOG_ERROR("editor.navigation_document", "%s requires resolving the navigation document group: %s", operation,
                  closed.ErrorValue().message.c_str());
        return false;
    }

    /** @copydoc EditorWorkspaceController::NavigationSettings */
    Result<NavigationProjectSettingsSession *> EditorWorkspaceController::NavigationSettings() {
        try {
            using Return = Result<NavigationProjectSettingsSession *>;
            if (m_navigationSettings)
                return Return::Success(m_navigationSettings.get());
            if (!m_mutableAssetRegistry || !m_durableFiles || !m_mutations || m_navigationOpening == UINT64_MAX)
                return Return::Failure(
                    MakeError(Navigation::NavigationSettingsError(Navigation::NavigationSettingsFailure::IncompleteInventory)));
            const auto &root = m_sourceOpenService.ProjectRoot();
            if (m_defaultScenePath) {
                const auto portable = m_defaultScenePath->lexically_relative(root).generic_u8string();
                const std::string path{portable.begin(), portable.end()};
                const auto registry = m_mutableAssetRegistry->Snapshot();
                const auto *record = registry.FindByPath(path);
                if (record) {
                    auto scene = SceneDocumentTransactions::Bind(m_document, m_history, {record->id, m_navigationOpening});
                    if (scene.HasError())
                        return Return::Failure(scene.ErrorValue());
                    m_navigationScene = std::make_unique<SceneDocumentTransactions>(std::move(scene).Value());
                }
            }
            auto session = NavigationProjectSettingsSession::Open(root, *m_mutableAssetRegistry, *m_durableFiles, *m_mutations,
                                                                  m_navigationOpening++, m_navigationScene.get());
            if (session.HasError()) {
                m_navigationScene.reset();
                return Return::Failure(session.ErrorValue());
            }
            m_navigationSettings = std::move(session).Value();
            return Return::Success(m_navigationSettings.get());
        } catch (const std::filesystem::filesystem_error &) {
            return Result<NavigationProjectSettingsSession *>::Failure(
                MakeError(Navigation::NavigationSettingsError(Navigation::NavigationSettingsFailure::Invalid)));
        }
    }

    /** @copydoc EditorWorkspaceController::HasDirtyNavigationDocuments */
    bool EditorWorkspaceController::HasDirtyNavigationDocuments() const {
        return m_navigationSettings && m_navigationSettings->IsDirty();
    }

    /** @copydoc EditorWorkspaceController::SaveNavigationDocuments */
    Result<void> EditorWorkspaceController::SaveNavigationDocuments() {
        if (!m_navigationSettings)
            return Result<void>::Success();
        auto saved = m_navigationSettings->SaveDocuments();
        if (saved.HasError())
            return Result<void>::Failure(saved.ErrorValue());
        for (const auto &outcome : saved.Value())
            if (outcome.error)
                return Result<void>::Failure(*outcome.error);
        if (m_defaultScenePath) {
            const auto current = LoadProjectScene(m_sourceOpenService.ProjectRoot(), *m_defaultScenePath);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            m_sceneFingerprint = current.Value().fingerprint;
            if (m_sceneFileWatch)
                m_sceneFileWatch->Reset();
        }
        UpdateNavigationSettingsProjection();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ShutdownNavigationDocuments */
    Result<void> EditorWorkspaceController::ShutdownNavigationDocuments(const bool discard) {
        if (!discard && HasDirtyNavigationDocuments())
            return Result<void>::Failure(MakeError(Navigation::NavigationSettingsError(Navigation::NavigationSettingsFailure::Referenced)));
        if (m_navigationSettings)
            m_navigationSettings->Close();
        for (const auto &tab : m_viewModel.navigationDocuments)
            static_cast<void>(
                m_viewModel.workspacePanelHost.CloseDocument(tab.identity.instance, WorkspaceDocumentClosePolicy::DiscardChanges));
        m_viewModel.navigationDocuments.clear();
        m_navigationSettings.reset();
        m_navigationScene.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::UpdateNavigationSettingsProjection */
    void EditorWorkspaceController::UpdateNavigationSettingsProjection() {
        if (!m_navigationSettings)
            return;
        RefreshNavigationInspectorDefinitions();
        if (const auto asset = m_navigationSettings->TakeOpenDefinitionRequest())
            OpenNavigationDefinitionTab(*asset);
        UpdateNavigationDocumentTabs();
        if (m_viewModel.documentRevision != m_document.Revision()) {
            RefreshSceneProjections();
            QueueRuntimeScene(m_document.Snapshot());
        }
        m_viewModel.isDirty = m_document.IsDirty();
    }

    /** @copydoc EditorWorkspaceController::OpenNavigationDefinitionTab */
    void EditorWorkspaceController::OpenNavigationDefinitionTab(const Assets::AssetId asset) {
        const auto registry = m_mutableAssetRegistry->Snapshot();
        const auto *record = registry.Find(asset);
        if (record && record->type.Value() == Assets::NavMeshAssetTypeName) {
            const auto source = SourceDocumentId::Parse(record->sourcePath.String());
            if (source.HasValue()) {
                const auto opened = m_viewModel.workspacePanelHost.OpenDocument({DocumentKind::Asset, source.Value()});
                if (opened.HasValue() && std::ranges::none_of(m_viewModel.navigationDocuments, [&](const auto &tab) {
                    return tab.asset == asset;
                })) {
                    auto editor = std::make_shared<NavigationDefinitionEditorState>();
                    editor->session = m_navigationSettings->Borrow();
                    m_viewModel.navigationDocuments.emplace_back(opened.Value().identity, asset, std::move(editor));
                }
            }
        }
    }

    /** @copydoc EditorWorkspaceController::UpdateNavigationDocumentTabs */
    void EditorWorkspaceController::UpdateNavigationDocumentTabs() {
        for (auto it = m_viewModel.navigationDocuments.begin(); it != m_viewModel.navigationDocuments.end();) {
            const auto &tab = *it;
            if (std::exchange(tab.editor->requestFocus, false))
                static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(tab.identity.instance));
            const bool dirty = m_navigationSettings->IsDirty();
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(tab.identity.instance, dirty));
            if (std::exchange(tab.editor->requestClose, false)) {
                const auto closed = m_viewModel.workspacePanelHost.CloseDocument(tab.identity.instance);
                if (closed.HasValue()) {
                    it = m_viewModel.navigationDocuments.erase(it);
                    continue;
                }
                tab.editor->status = closed.ErrorValue().message;
            }
            ++it;
        }
    }
}  // namespace Horo::Editor
