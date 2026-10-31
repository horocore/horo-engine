#include "EditorWorkspaceController.h"
#include "terrain/TerrainDocumentHost.h"
#include "Horo/Foundation/Logging/Logger.h"
#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Compare logical document currentness without disk I/O or live worker access. */
        bool SameCookSource(const TerrainDocumentView &view, const Application::TerrainAuthoringCookFence &fence) {
            return !view.closed && !view.dirty && view.workspace == fence.workspace && view.fence.session == fence.session &&
                view.fence.dataset == fence.dataset && view.fence.expectedRevision == fence.source && view.fence.capabilityRevision == fence.capability;
        }
    }
    /** @copydoc EditorWorkspaceController::EnsureTerrainCook */
    Result<void> EditorWorkspaceController::EnsureTerrainCook(const std::filesystem::path &root) {
        if (m_terrainCook) return Result<void>::Success();
        if (!m_terrainCookFactory || !*m_terrainCookFactory)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        try {
            auto composed = (*m_terrainCookFactory)(root);
            if (composed.HasError()) return Result<void>::Failure(composed.ErrorValue());
            if (!composed.Value().host) return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            m_terrainCook.emplace(std::move(composed).Value());
            return Result<void>::Success();
        } catch (...) {
            return Result<void>::Failure(MakeError(Application::TerrainAuthoringCookErrors::Failed));
        }
    }
    /** @copydoc EditorWorkspaceController::StartTerrainCook */
    Result<void> EditorWorkspaceController::StartTerrainCook(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        if (m_viewModel.terrainCookEpoch == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(TerrainEditErrors::Exhausted));
        auto saved = document.CaptureSavedSource(intent.document.fence);
        if (saved.HasError()) return Result<void>::Failure(saved.ErrorValue());
        if (auto composed = EnsureTerrainCook(saved.Value().root); composed.HasError()) return composed;
        const auto &capture = saved.Value();
        Application::TerrainAuthoringCookFence fence{capture.workspace,capture.fence.session,capture.fence.dataset,
            capture.fence.expectedRevision,capture.fence.capabilityRevision,capture.sourceDigest,capture.layerDigest};
        Application::TerrainAuthoringCookRequest request{capture.root,m_terrainCook->cookedRoot,capture.asset,fence,
            capture.metadataDigest,m_terrainCook->profile};
        if (auto started = m_terrainCook->host->Start(std::move(request)); started.HasError()) return started;
        m_terrainCookFence = fence; m_terrainCookDocument = intent.document.instance; m_terrainCookAdoption = true;
        m_terrainCookRegistry = m_assetRegistry.Revision();
        ++m_viewModel.terrainCookEpoch; m_viewModel.terrainCookRunning = true; m_viewModel.terrainCookCurrent = false;
        m_viewModel.terrainCookDocument = intent.document.instance;
        m_viewModel.terrainCookOutcome.reset();
        return Result<void>::Success();
    }
    /** @copydoc EditorWorkspaceController::ExecuteTerrainCook */
    Result<void> EditorWorkspaceController::ExecuteTerrainCook(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        if (intent.action == TerrainToolAction::Cook) return StartTerrainCook(document,intent);
        if (intent.cookEpoch != m_viewModel.terrainCookEpoch) return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        CancelTerrainCook(m_terrainCookDocument);
        return Result<void>::Success();
    }
    /** @copydoc EditorWorkspaceController::CancelTerrainCook */
    void EditorWorkspaceController::CancelTerrainCook(DocumentInstanceId instance) noexcept {
        if (!m_terrainCook || m_terrainCookDocument != instance) return;
        m_terrainCookAdoption = false; m_viewModel.terrainCookCurrent = false; m_terrainCook->host->Cancel();
    }
    /** @copydoc EditorWorkspaceController::TerrainCookFenceMatches */
    bool EditorWorkspaceController::TerrainCookFenceMatches() const {
        if (!m_terrainCookFence || m_assetRegistry.Revision() != m_terrainCookRegistry) return false;
        const auto found = std::ranges::find_if(m_terrainDocuments,[this](const auto &document) {
            return document->Identity().instance == m_terrainCookDocument;
        });
        if (found == m_terrainDocuments.end()) return false;
        return SameCookSource((*found)->View(),*m_terrainCookFence);
    }
    /** @copydoc EditorWorkspaceController::UpdateTerrainCook */
    void EditorWorkspaceController::UpdateTerrainCook() {
        if (!m_terrainCook) return;
        if (m_terrainCookAdoption && !TerrainCookFenceMatches()) CancelTerrainCook(m_terrainCookDocument);
        auto completed = m_terrainCook->host->Poll();
        m_viewModel.terrainCookRunning = m_terrainCook->host->IsRunning();
        if (!completed) return;
        m_viewModel.terrainCookCurrent = m_terrainCookAdoption && TerrainCookFenceMatches() &&
            completed->fence == *m_terrainCookFence && completed->result.HasValue();
        if (completed->result.HasError()) {
            m_viewModel.terrainDocumentError = completed->result.ErrorValue();
            LOG_WARN("editor.terrain","%s: %s",completed->result.ErrorValue().code.Value().c_str(),completed->result.ErrorValue().message.c_str());
        }
        m_viewModel.terrainCookOutcome = std::make_shared<const Application::TerrainAuthoringCookCompletion>(std::move(*completed));
    }
    /** @copydoc EditorWorkspaceController::ShutdownTerrainCook */
    void EditorWorkspaceController::ShutdownTerrainCook() noexcept {
        if (!m_terrainCook) return;
        m_terrainCookAdoption = false; m_terrainCook->host->Retire(); m_terrainCook->host->Drain();
        m_terrainCook.reset();
    }
}
