#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::UpdateAutosave */
    void EditorWorkspaceController::UpdateAutosave(const float elapsedSeconds, const int intervalMinutes) {
        if (intervalMinutes <= 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F || !m_document.IsDirty() ||
            m_viewModel.recoveryAvailable || m_autosaveSuppressedForDiscard) {
            if (!m_document.IsDirty())
                m_autosaveElapsedSeconds = 0.0F;
            return;
        }
        if (m_document.State() == m_lastAutosavedState)
            return;
        if (m_autosaveRetryDelaySeconds > 0.0F) {
            m_autosaveRetryDelaySeconds = std::max(0.0F, m_autosaveRetryDelaySeconds - elapsedSeconds);
            if (m_autosaveRetryDelaySeconds > 0.0F)
                return;
        }

        m_autosaveElapsedSeconds += elapsedSeconds;
        const float intervalSeconds = static_cast<float>(intervalMinutes) * 60.0F;
        if (m_autosaveElapsedSeconds >= intervalSeconds)
            WriteAutosaveRecovery();
    }

    /** @copydoc EditorWorkspaceController::FlushAutosave */
    void EditorWorkspaceController::FlushAutosave() {
        if (m_document.IsDirty() && !m_viewModel.recoveryAvailable && !m_autosaveSuppressedForDiscard &&
            m_document.State() != m_lastAutosavedState)
            WriteAutosaveRecovery();
    }

    /** @copydoc EditorWorkspaceController::WriteAutosaveRecovery */
    void EditorWorkspaceController::WriteAutosaveRecovery() {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr)
            return;
        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        if (const Result<void> written =
                WriteProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath, snapshot,
                                          m_document.SavedRevision(), m_document.SavedState(), *m_mutations, *m_durableFiles);
            written.HasError()) {
            m_autosaveRetryDelaySeconds = 30.0F;
            LOG_ERROR("editor.scene_recovery", "Autosave recovery write failed: %s", written.ErrorValue().message.c_str());
            return;
        }
        m_lastAutosavedState = snapshot.state;
        m_autosaveElapsedSeconds = 0.0F;
        m_autosaveRetryDelaySeconds = 0.0F;
        LOG_INFO("editor.scene_recovery", "Autosaved recovery revision %llu for '%s'.",
                 static_cast<unsigned long long>(snapshot.revision.value), m_defaultScenePath->string().c_str());
    }

    bool EditorWorkspaceController::CommitSceneSave(const SceneDocumentSnapshot &snapshot, const SceneFileFingerprint &fingerprint,
                                                    const std::optional<std::filesystem::path> &newActivePath) {
        if (const Result<void> marked = m_document.MarkSaved(snapshot.revision, snapshot.state); marked.HasError()) {
            LOG_ERROR("editor.scene_document", "Saved scene state could not be acknowledged: %s", marked.ErrorValue().message.c_str());
            return false;
        }
        if (newActivePath.has_value())
            m_defaultScenePath = *newActivePath;
        m_sceneFingerprint = fingerprint;
        if (m_sceneFileWatch != nullptr)
            m_sceneFileWatch->Reset();
        m_viewModel.sceneExternalConflict = false;
        m_sceneFileWatchErrorPresented = false;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        if (const Result<void> recoveryDiscarded =
                DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
            recoveryDiscarded.HasError()) {
            LOG_WARN("editor.scene_recovery", "Saved scene but could not clean recovery state: %s",
                     recoveryDiscarded.ErrorValue().message.c_str());
        } else {
            m_viewModel.recoveryAvailable = false;
            m_lastAutosavedState = {};
        }
        m_autosaveSuppressedForDiscard = false;
        m_dataBus.Publish(SceneDocumentChangedEvent{m_document.Revision(),
                                                    m_document.State(),
                                                    DocumentChangeKind::SaveStateChanged,
                                                    m_document.IsDirty(),
                                                    {}});
        RefreshSceneProjections();
        return true;
    }

    /** @copydoc EditorWorkspaceController::SaveScene */
    void EditorWorkspaceController::SaveScene(const bool overwriteConflict) {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr) {
            LOG_ERROR("editor.scene_document", "Save rejected because the project scene path or "
                                               "durable writer services are unavailable.");
            return;
        }

        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        if (!m_sceneFingerprint.has_value()) {
            LOG_ERROR("editor.scene_document", "Save rejected because the canonical scene identity is unavailable.");
            return;
        }
        const Result<ProjectSceneSaveResult> saved =
            SaveProjectScene(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath, snapshot, *m_sceneFingerprint,
                             overwriteConflict, *m_mutations, *m_durableFiles);
        if (saved.HasError()) {
            LOG_ERROR("editor.scene_document", "Scene save failed for '%s': %s", m_defaultScenePath->string().c_str(),
                      saved.ErrorValue().message.c_str());
            return;
        }
        if (saved.Value().status == ProjectSceneSaveStatus::Conflict) {
            m_viewModel.sceneExternalConflict = true;
            LOG_WARN("editor.scene_document", "Save paused because '%s' changed outside this document session.",
                     m_defaultScenePath->string().c_str());
            return;
        }

        if (!CommitSceneSave(snapshot, saved.Value().fingerprint))
            return;
        LOG_INFO("editor.scene_document", "Saved scene revision %llu to '%s'.", static_cast<unsigned long long>(snapshot.revision.value),
                 m_defaultScenePath->string().c_str());
    }

    std::optional<std::filesystem::path> EditorWorkspaceController::ResolveSceneSaveDestination(
        const std::filesystem::path &absolutePath) const {
        std::error_code canonicalError;
        const std::filesystem::path destination = std::filesystem::weakly_canonical(absolutePath, canonicalError);
        if (canonicalError) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' could not be normalized: %s",
                      absolutePath.string().c_str(), canonicalError.message().c_str());
            return std::nullopt;
        }
        if (!destination.is_absolute()) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' is not absolute.", destination.string().c_str());
            return std::nullopt;
        }
        return destination;
    }

    /** @copydoc EditorWorkspaceController::SaveSceneToPath */
    void EditorWorkspaceController::SaveSceneToPath(const std::filesystem::path &absolutePath, const bool copyOnly) {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because the active scene or "
                                               "durable writer services are unavailable.");
            return;
        }

        const std::optional<std::filesystem::path> destination = ResolveSceneSaveDestination(absolutePath);
        if (!destination.has_value())
            return;
        const std::filesystem::path activePath = m_defaultScenePath->lexically_normal();
        if (copyOnly && *destination == activePath) {
            LOG_ERROR("editor.scene_document", "Save Copy As rejected because the destination is the active scene path '%s'.",
                      activePath.string().c_str());
            return;
        }
        if (!copyOnly && *destination == activePath) {
            SaveScene(true);
            return;
        }

        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        auto saved = SaveProjectSceneToPath(std::filesystem::path{m_viewModel.projectRoot}, *destination, snapshot, true, *m_mutations,
                                            *m_durableFiles);
        if (saved.HasError()) {
            LOG_ERROR("editor.scene_document", "%s failed for '%s': %s", copyOnly ? "Save Copy As" : "Save As",
                      destination->string().c_str(), saved.ErrorValue().message.c_str());
            return;
        }
        if (saved.Value().status != ProjectSceneDestinationSaveStatus::Saved) {
            LOG_WARN("editor.scene_document", "%s paused because destination '%s' changed during the save.",
                     copyOnly ? "Save Copy As" : "Save As", destination->string().c_str());
            return;
        }

        if (copyOnly) {
            LOG_INFO("editor.scene_document", "Saved scene copy revision %llu to '%s'.",
                     static_cast<unsigned long long>(snapshot.revision.value), destination->string().c_str());
            return;
        }

        if (!CommitSceneSave(snapshot, saved.Value().fingerprint, *destination))
            return;
        LOG_INFO("editor.scene_document", "Saved scene revision %llu as '%s'; active document identity was updated.",
                 static_cast<unsigned long long>(snapshot.revision.value), destination->string().c_str());
    }
}  // namespace Horo::Editor
