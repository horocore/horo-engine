#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <utility>

namespace Horo::Editor {

    /** @copydoc EditorWorkspaceController::ReloadExternalScene */
    void EditorWorkspaceController::ReloadExternalScene() {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr)
            return;

        if (const Result<LoadedProjectScene> loaded = LoadProjectScene(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
            loaded.HasError() || !loaded.Value().existed) {
            LOG_ERROR("editor.scene_document", "External scene reload failed because the canonical document is unavailable.");
            return;
        } else {
            LoadedProjectScene external = loaded.Value();
            SceneDocument validatedExternal;
            if (const Result<void> validated = validatedExternal.LoadSaved(external.objects, external.prefabInstances);
                validated.HasError()) {
                LOG_ERROR("editor.scene_document", "External scene validation failed: %s", validated.ErrorValue().message.c_str());
                return;
            }
            if (const Result<void> recoveryDiscarded =
                    DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
                recoveryDiscarded.HasError()) {
                LOG_ERROR("editor.scene_recovery", "External reload could not discard superseded recovery state: %s",
                          recoveryDiscarded.ErrorValue().message.c_str());
                return;
            }
            if (const Result<void> installed = m_document.LoadSaved(std::move(external.objects), std::move(external.prefabInstances));
                installed.HasError()) {
                LOG_ERROR("editor.scene_document", "External scene validation failed: %s", installed.ErrorValue().message.c_str());
                return;
            }

            m_sceneFingerprint = std::move(external.fingerprint);
        }
        if (m_sceneFileWatch != nullptr)
            m_sceneFileWatch->Reset();
        m_history.Clear();
        m_selection.Clear();
        m_deferredRuntimeSnapshot.reset();
        m_activeRuntimeRevision = {};
        m_queuedDefinitionRevision = {};
        m_viewModel.sceneExternalConflict = false;
        m_sceneFileWatchErrorPresented = false;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = {};
        m_autosaveElapsedSeconds = 0.0F;
        m_autosaveRetryDelaySeconds = 0.0F;
        m_autosaveSuppressedForDiscard = false;
        RefreshSceneProjections();
        LOG_INFO("editor.scene_document", "Reloaded externally changed scene '%s'.", m_defaultScenePath->string().c_str());
    }

    /** @copydoc EditorWorkspaceController::RestoreSceneRecovery */
    void EditorWorkspaceController::RestoreSceneRecovery() {
        if (!m_defaultScenePath.has_value())
            return;
        Result<std::optional<ProjectSceneRecoveryRecord>> recovery =
            InspectProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
        if (recovery.HasError() || !recovery.Value().has_value()) {
            LOG_ERROR("editor.scene_recovery", "Recovery restore failed because no valid record is available.");
            return;
        }
        std::optional<ProjectSceneRecoveryRecord> recoveryRecord = std::move(recovery).Value();
        if (const Result<void> restored =
                m_document.LoadRecovered(std::move(recoveryRecord->objects), std::move(recoveryRecord->prefabInstances));
            restored.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery restore validation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_history.Clear();
        m_selection.Clear();
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = m_document.State();
        m_autosaveSuppressedForDiscard = false;
        RefreshSceneProjections();
        LOG_INFO("editor.scene_recovery", "Recovery restored into a new dirty document session.");
    }

    /** @copydoc EditorWorkspaceController::DiscardSceneRecovery */
    void EditorWorkspaceController::DiscardSceneRecovery() {
        if (m_mutations == nullptr || m_durableFiles == nullptr)
            return;
        if (const Result<void> discarded =
                DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
            discarded.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery discard failed: %s", discarded.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = {};
        m_autosaveSuppressedForDiscard = true;
        LOG_INFO("editor.scene_recovery", "Recovery state discarded explicitly.");
    }

}  // namespace Horo::Editor
