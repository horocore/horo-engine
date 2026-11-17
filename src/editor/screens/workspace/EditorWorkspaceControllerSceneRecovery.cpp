#include "Horo/Foundation/Logging/Logger.h"
#include "WorkspacePathText.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Missing external source is a replacement failure, never permission to clear the document. */
        const ErrorCodeDescriptor ExternalSceneUnavailable{.domain = ErrorDomainId{"editor.scene_document"},
                                                           .code = ErrorCode{"external_scene_unavailable"},
                                                           .defaultSeverity = ErrorSeverity::Error,
                                                           .summary = "The canonical external Scene is unavailable.",
                                                           .remediationHint = "Restore the Scene file or choose another document."};

        /** @brief Discards superseded recovery state before any canonical Scene owner replacement. */
        bool DiscardSupersededRecovery(const std::filesystem::path &root, const ProjectMutationCoordinator &mutations,
                                       DurableFileSystem &files) {
            if (const Result<void> recoveryDiscarded = DiscardProjectSceneRecovery(root, mutations, files); recoveryDiscarded.HasError()) {
                LOG_ERROR("editor.scene_recovery", "External reload could not discard superseded recovery state: %s",
                          recoveryDiscarded.ErrorValue().message.c_str());
                return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::ReloadExternalScene */
    void EditorWorkspaceController::ReloadExternalScene() {
        if (DeferPlaySceneReplacement(PlaySceneReplacementKind::External))
            return;
        if (!PrepareNavigationSceneReplacement("Scene replacement"))
            return;

        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr)
            return;

        auto loaded = ReadExternalSceneForReplacement();
        if (loaded.HasError()) {
            LOG_ERROR("editor.scene_document", "%s", loaded.ErrorValue().message.c_str());
            return;
        }
        if (!InstallExternalScene(std::move(loaded).Value()))
            return;
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
        LOG_INFO("editor.scene_document", "Reloaded externally changed scene '%s'.", WorkspacePathText(*m_defaultScenePath).c_str());
    }

    /** @copydoc EditorWorkspaceController::ReadExternalSceneForReplacement */
    Result<LoadedProjectScene> EditorWorkspaceController::ReadExternalSceneForReplacement() const {
        auto loaded = LoadProjectScene(ProjectRoot(), *m_defaultScenePath);
        if (loaded.HasError())
            return loaded;
        if (!loaded.Value().existed)
            return Result<LoadedProjectScene>::Failure(MakeError(ExternalSceneUnavailable));
        SceneDocument validated;
        if (const auto result = validated.LoadSaved(loaded.Value().objects, loaded.Value().prefabInstances); result.HasError())
            return Result<LoadedProjectScene>::Failure(result.ErrorValue());
        return loaded;
    }

    /** @copydoc EditorWorkspaceController::InstallExternalScene */
    bool EditorWorkspaceController::InstallExternalScene(LoadedProjectScene scene) {
        if (HasDirtyCollisionSettings()) {
            LOG_ERROR("editor.collision", "Save or discard collision settings before reloading the Scene owner.");
            return false;
        }
        if (!DiscardSupersededRecovery(ProjectRoot(), *m_mutations, *m_durableFiles))
            return false;
        if (auto closed = ShutdownCollisionSettings(); closed.HasError())
            return false;
        if (const auto installed = m_document.LoadSaved(std::move(scene.objects), std::move(scene.prefabInstances)); installed.HasError()) {
            LOG_ERROR("editor.scene_document", "External scene validation failed: %s", installed.ErrorValue().message.c_str());
            return false;
        }
        m_sceneFingerprint = std::move(scene.fingerprint);
        return true;
    }

    /** @copydoc EditorWorkspaceController::RestoreSceneRecovery */
    void EditorWorkspaceController::RestoreSceneRecovery() {
        if (DeferPlaySceneReplacement(PlaySceneReplacementKind::Recovery))
            return;
        if (!PrepareNavigationSceneReplacement("Scene replacement"))
            return;

        if (!m_defaultScenePath.has_value())
            return;
        Result<std::optional<ProjectSceneRecoveryRecord>> recovery = InspectProjectSceneRecovery(ProjectRoot(), *m_defaultScenePath);
        if (recovery.HasError() || !recovery.Value().has_value()) {
            LOG_ERROR("editor.scene_recovery", "Recovery restore failed because no valid record is available.");
            return;
        }
        std::optional<ProjectSceneRecoveryRecord> recoveryRecord = std::move(recovery).Value();
        SceneDocument validatedRecovery;
        if (auto validated = validatedRecovery.LoadRecovered(recoveryRecord->objects, recoveryRecord->prefabInstances);
            validated.HasError())
            return;
        if (auto closed = ShutdownCollisionSettings(); closed.HasError()) {
            LOG_ERROR("editor.collision", "%s", closed.ErrorValue().message.c_str());
            return;
        }
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
        if (const Result<void> discarded = DiscardProjectSceneRecovery(ProjectRoot(), *m_mutations, *m_durableFiles);
            discarded.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery discard failed: %s", discarded.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = {};
        m_autosaveSuppressedForDiscard = true;
        LOG_INFO("editor.scene_recovery", "Recovery state discarded explicitly.");
    }

    /** @copydoc EditorWorkspaceController::DeferPlaySceneReplacement */
    bool EditorWorkspaceController::DeferPlaySceneReplacement(PlaySceneReplacementKind kind) {
        if (m_playSession.State() == EditorPlaySessionState::Idle)
            return false;
        m_playSceneReplacement = PlaySceneReplacementIntent{kind, m_document.Revision(), m_document.ReplacementEpoch(), m_defaultScenePath};
        StopPlaySession();
        return true;
    }

    /** @copydoc EditorWorkspaceController::PlaySceneReplacementMatches */
    bool EditorWorkspaceController::PlaySceneReplacementMatches() const noexcept {
        return m_playSceneReplacement && m_playSceneReplacement->revision == m_document.Revision() &&
               m_playSceneReplacement->replacementEpoch == m_document.ReplacementEpoch() &&
               m_playSceneReplacement->scenePath == m_defaultScenePath;
    }

    /** @copydoc EditorWorkspaceController::ResumePlaySceneReplacement */
    void EditorWorkspaceController::ResumePlaySceneReplacement() {
        if (!m_playSceneReplacement)
            return;
        if (!PlaySceneReplacementMatches()) {
            m_playSceneReplacement.reset();
            LOG_WARN("editor.play.save", "Scene replacement consent expired while Play was stopping; request replacement again.");
            return;
        }
        if (m_playSession.State() != EditorPlaySessionState::Idle)
            return;
        const auto kind = m_playSceneReplacement->kind;
        m_playSceneReplacement.reset();
        if (kind == PlaySceneReplacementKind::External)
            ReloadExternalScene();
        else
            RestoreSceneRecovery();
    }

}  // namespace Horo::Editor
