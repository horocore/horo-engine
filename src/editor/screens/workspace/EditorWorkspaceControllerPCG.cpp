#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Captures a current source command header, separate from tab/widget identity. */
        PCGDocumentCommand Fence(const PCGDocumentSnapshot &snapshot) {
            return {.session = snapshot.session,
                    .expectedRevision = snapshot.revision,
                    .catalogRevision = snapshot.catalogRevision,
                    .canEdit = true};
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::OpenEmbeddedPCGSource */
    void EditorWorkspaceController::OpenEmbeddedPCGSource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        const auto *asset = m_assetRegistry.FindByPath(result.location.document.Value());
        if (!asset) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unsupported";
            return;
        }
        auto document = m_pcgDocuments.Open(asset->id, opened.identity, result.location.absolutePath);
        if (document.HasError()) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            LOG_ERROR("editor.pcg_document", "Graph source admission failed: %s", document.ErrorValue().message.c_str());
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
            return;
        }
        const auto &snapshot = document.Value();
        if (snapshot.identity.instance != opened.identity.instance) {
            static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(snapshot.identity.instance));
            return;
        }
        if (const auto tab = m_viewModel.workspacePanelHost.OpenDocument(snapshot.identity.key); tab.HasError()) {
            if (opened.disposition == DocumentOpenDisposition::Opened) {
                static_cast<void>(m_pcgDocuments.Close(opened.identity.instance, true));
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            }
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
            return;
        }
        RefreshPCGDocumentProjection(snapshot.identity.instance);
    }

    /** @copydoc EditorWorkspaceController::RefreshPCGDocumentProjection */
    void EditorWorkspaceController::RefreshPCGDocumentProjection(DocumentInstanceId instance) {
        const auto snapshot = m_pcgDocuments.Snapshot(instance);
        if (snapshot.HasValue())
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(instance, snapshot.Value().dirty));
    }

    /** @copydoc EditorWorkspaceController::ExecutePCGDocument */
    Result<void> EditorWorkspaceController::ExecutePCGDocument(DocumentInstanceId instance, const PCGDocumentCommand &command) {
        auto result = m_pcgDocuments.Execute(instance, command);
        if (result.HasValue())
            RefreshPCGDocumentProjection(instance);
        return result;
    }

    /** @copydoc EditorWorkspaceController::ReplayPCGDocument */
    Result<void> EditorWorkspaceController::ReplayPCGDocument(DocumentInstanceId instance, const PCGDocumentCommand &command, bool redo) {
        auto result = m_pcgDocuments.Replay(instance, command, redo);
        if (result.HasValue())
            RefreshPCGDocumentProjection(instance);
        return result;
    }

    /** @copydoc EditorWorkspaceController::SavePCGDocument */
    Result<Assets::AssetSourcePublication> EditorWorkspaceController::SavePCGDocument(DocumentInstanceId instance,
                                                                                      const PCGDocumentCommand &command) {
        auto result = m_pcgDocuments.Save(instance, command);
        RefreshPCGDocumentProjection(instance);
        return result;
    }

    /** @copydoc EditorWorkspaceController::ClosePCGDocument */
    Result<void> EditorWorkspaceController::ClosePCGDocument(DocumentInstanceId instance, bool discardDirty) {
        if (const auto tabs = m_viewModel.workspacePanelHost.DocumentTabs();
            std::ranges::none_of(tabs, [instance](const WorkspaceDocumentTab &tab) {
            return tab.identity.instance == instance;
        }))
            return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Stale)));
        if (const auto closed = m_pcgDocuments.Close(instance, discardDirty); closed.HasError())
            return closed;
        return m_viewModel.workspacePanelHost.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges);
    }

    /** @copydoc EditorWorkspaceController::HasDirtyPCGDocuments */
    bool EditorWorkspaceController::HasDirtyPCGDocuments() const {
        if (m_pcgDocumentsShutdown)
            return false;
        const auto snapshots = m_pcgDocuments.Snapshots();
        return snapshots.HasError() || std::ranges::any_of(snapshots.Value(), &PCGDocumentSnapshot::dirty);
    }

    /** @copydoc EditorWorkspaceController::SavePCGDocuments */
    Result<void> EditorWorkspaceController::SavePCGDocuments() {
        if (m_pcgDocumentsShutdown)
            return Result<void>::Success();
        auto snapshots = m_pcgDocuments.Snapshots();
        if (snapshots.HasError())
            return Result<void>::Failure(snapshots.ErrorValue());
        for (const auto &snapshot : snapshots.Value()) {
            if (!snapshot.dirty)
                continue;
            const auto publication = SavePCGDocument(snapshot.identity.instance, Fence(snapshot));
            if (publication.HasError())
                return Result<void>::Failure(publication.ErrorValue());
            if (publication.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(*publication.Value().durabilityError);
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ShutdownPCGDocuments */
    Result<void> EditorWorkspaceController::ShutdownPCGDocuments(bool discardDirty) {
        auto result = m_pcgDocuments.Shutdown(!discardDirty, discardDirty);
        if (result.HasValue())
            m_pcgDocumentsShutdown = true;
        return result;
    }

    /** @copydoc EditorWorkspaceController::UpdatePCGAutosave */
    void EditorWorkspaceController::UpdatePCGAutosave(float elapsedSeconds, int intervalMinutes) {
        if (m_pcgDocumentsShutdown || intervalMinutes <= 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0)
            return;
        if (!HasDirtyPCGDocuments()) {
            m_pcgAutosaveElapsedSeconds = 0;
            return;
        }
        if (m_pcgAutosaveRetrySeconds > 0) {
            m_pcgAutosaveRetrySeconds = std::max(0.0F, m_pcgAutosaveRetrySeconds - elapsedSeconds);
            if (m_pcgAutosaveRetrySeconds > 0)
                return;
        }
        m_pcgAutosaveElapsedSeconds += elapsedSeconds;
        if (m_pcgAutosaveElapsedSeconds < static_cast<float>(intervalMinutes) * 60.0F)
            return;
        if (const auto recovered = m_pcgDocuments.FlushRecovery(); recovered.HasError()) {
            m_pcgAutosaveRetrySeconds = 30;
            if (!m_pcgAutosaveErrorPresented)
                LOG_ERROR("editor.pcg_recovery", "Graph autosave failed: %s", recovered.ErrorValue().message.c_str());
            m_pcgAutosaveErrorPresented = true;
            return;
        }
        m_pcgAutosaveElapsedSeconds = 0;
        m_pcgAutosaveRetrySeconds = 0;
        m_pcgAutosaveErrorPresented = false;
    }
}  // namespace Horo::Editor
