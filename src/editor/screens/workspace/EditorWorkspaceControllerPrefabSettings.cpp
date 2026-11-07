#include "Horo/Prefab/PrefabErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::PrefabSettings */
    Result<std::shared_ptr<PrefabProjectSettingsSession>> EditorWorkspaceController::PrefabSettings() {
        using Return = Result<std::shared_ptr<PrefabProjectSettingsSession>>;
        if (m_prefabSettings)
            return Return::Success(m_prefabSettings);
        if (!m_mutations || !m_durableFiles)
            return Return::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
        auto opened = PrefabProjectSettingsSession::Open(m_sourceOpenService.ProjectRoot(), *m_durableFiles, *m_mutations);
        if (opened.HasError())
            return Return::Failure(std::move(opened).ErrorValue());
        m_prefabSettings = std::move(opened).Value();
        m_prefabSettingsPublication = m_prefabSettings->Snapshot().Value()->publication;
        return Return::Success(m_prefabSettings);
    }

    /** @copydoc EditorWorkspaceController::PrefabLimits */
    Result<Prefab::PrefabLimitProfile> EditorWorkspaceController::PrefabLimits() {
        if (!m_durableFiles || !m_mutations) {
            // Read-only/headless hosts have no editing authority, but consume the identical project document projection.
            auto captured = Application::LoadPrefabProjectSettings(m_sourceOpenService.ProjectRoot());
            if (captured.HasError())
                return Result<Prefab::PrefabLimitProfile>::Failure(std::move(captured).ErrorValue());
            return Result<Prefab::PrefabLimitProfile>::Success(captured.Value().settings->Limits());
        }
        auto owner = PrefabSettings();
        if (owner.HasError())
            return Result<Prefab::PrefabLimitProfile>::Failure(std::move(owner).ErrorValue());
        auto accepted = owner.Value()->Accepted();
        if (accepted.HasError())
            return Result<Prefab::PrefabLimitProfile>::Failure(std::move(accepted).ErrorValue());
        return Result<Prefab::PrefabLimitProfile>::Success(accepted.Value()->Limits());
    }

    /** @copydoc EditorWorkspaceController::UpdatePrefabSettingsProjection */
    void EditorWorkspaceController::UpdatePrefabSettingsProjection() {
        if (!m_prefabSettings)
            return;
        const auto snapshot = m_prefabSettings->Snapshot();
        if (snapshot.HasError() || snapshot.Value()->publication == m_prefabSettingsPublication)
            return;
        m_prefabSettingsPublication = snapshot.Value()->publication;
        CancelAssetPlacementPreview();
        RetirePrefabHierarchy();
        m_navigationPrefabSchemas.reset();
        ScheduleContentBrowserPreviews();
        const auto accepted = m_prefabSettings->Accepted();
        if (accepted.HasValue()) {
            for (const auto &document : m_prefabDocuments) {
                const auto checked = document->UpdateLimitProfile(accepted.Value()->Limits());
                if (checked.HasError())
                    m_viewModel.prefabDocumentError = checked.ErrorValue();
            }
        }
        // Rebuild source inspection only. Current Scene/stage runtimes and running operations retain their captured profile.
        RefreshSceneProjections();
    }

    /** @copydoc EditorWorkspaceController::HasDirtyPrefabSettings */
    bool EditorWorkspaceController::HasDirtyPrefabSettings() const {
        return m_prefabSettings && m_prefabSettings->IsDirty();
    }

    /** @copydoc EditorWorkspaceController::PrefabSettingsRevision */
    std::uint64_t EditorWorkspaceController::PrefabSettingsRevision() const {
        if (!m_prefabSettings)
            return 0;
        const auto view = m_prefabSettings->Snapshot();
        return view.HasValue() ? view.Value()->revision : 0;
    }

    /** @copydoc EditorWorkspaceController::SavePrefabSettings */
    Result<void> EditorWorkspaceController::SavePrefabSettings() {
        if (!HasDirtyPrefabSettings())
            return Result<void>::Success();
        const auto snapshot = m_prefabSettings->Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        auto saved = m_prefabSettings->Execute({PrefabSettingsCommandKind::Save, snapshot.Value()->revision});
        if (saved.HasError())
            return Result<void>::Failure(std::move(saved).ErrorValue());
        const auto published = m_prefabSettings->Snapshot();
        if (published.Value()->durabilityError)
            return Result<void>::Failure(*published.Value()->durabilityError);
        UpdatePrefabSettingsProjection();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ShutdownPrefabSettings */
    Result<void> EditorWorkspaceController::ShutdownPrefabSettings(const bool discard) {
        if (!discard && HasDirtyPrefabSettings())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        if (m_prefabSettings)
            m_prefabSettings->Close();
        m_prefabSettings.reset();
        m_prefabSettingsPublication = 0;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
