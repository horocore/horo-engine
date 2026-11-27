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
        if (snapshot.HasError())
            return;
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(instance, snapshot.Value().dirty));
        auto &documents = m_viewModel.pcgDocuments;
        auto found = std::ranges::find(documents, instance, [](const auto &entry) {
            return entry.snapshot.identity.instance;
        });
        if (found == documents.end()) {
            documents.emplace_back();
            found = std::prev(documents.end());
        }
        RefreshPCGCanvas(*found, snapshot.Value());
        found->previewAvailable = m_pcgPreviewJobs && m_pcgPreviewInputs && HasPCGPreviewGrant();
        UpdatePCGPreviews();
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
        RetirePCGPreview(instance);
        std::erase_if(m_viewModel.pcgDocuments, [instance](const auto &entry) {
            return entry.snapshot.identity.instance == instance;
        });
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
        if (result.HasValue()) {
            m_pcgPreviews.clear();  // Sessions cancel and join before workspace roots retire.
            m_pcgDocumentsShutdown = true;
            m_viewModel.pcgDocuments.clear();
        }
        return result;
    }

    /** @copydoc EditorWorkspaceController::UpdatePCGAutosave */
    void EditorWorkspaceController::UpdatePCGAutosave(float elapsedSeconds, int intervalMinutes) {
        UpdatePCGPreviews();
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

    /** @copydoc EditorWorkspaceController::HasActivePCGDocument */
    bool EditorWorkspaceController::HasActivePCGDocument() const noexcept {
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        return std::ranges::any_of(m_viewModel.pcgDocuments, [active](const auto &entry) {
            return active == entry.snapshot.identity.instance;
        });
    }

    /** @copydoc EditorWorkspaceController::ProcessPCGDocumentShortcut */
    bool EditorWorkspaceController::ProcessPCGDocumentShortcut(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command != UndoScene && command.command != RedoScene && command.command != SaveScene &&
            command.command != DeleteSelectedObjects)
            return false;
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        const auto found = std::ranges::find_if(m_viewModel.pcgDocuments, [active](const auto &entry) {
            return active == entry.snapshot.identity.instance;
        });
        if (found == m_viewModel.pcgDocuments.end())
            return false;
        EditorWorkspaceViewCommandData routed;
        routed.command = EditPCGGraph;
        routed.documentInstance = found->snapshot.identity.instance;
        auto action = PCGEditorAction::Save;
        if (command.command == UndoScene)
            action = PCGEditorAction::Undo;
        else if (command.command == RedoScene)
            action = PCGEditorAction::Redo;
        else if (command.command == DeleteSelectedObjects)
            action = PCGEditorAction::RemoveSelection;
        routed.pcgRequest = PCGEditorRequest{.action = action, .fence = Fence(found->snapshot), .presentation = found->canvas};
        return ProcessPCGDocumentCommand(routed);
    }

    /** @copydoc EditorWorkspaceController::ApplyPCGEditorRequest */
    Result<void> EditorWorkspaceController::ApplyPCGEditorRequest(PCGWorkspaceDocument &document, const PCGEditorRequest &request) {
        using enum PCGDocumentCommandFailureReason;
        const auto instance = document.snapshot.identity.instance;
        const auto current = m_pcgDocuments.Snapshot(instance);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &snapshot = current.Value();
        const auto &fence = request.fence;
        if (m_viewModel.workspacePanelHost.ActiveDocument() != instance || snapshot.session != fence.session ||
            snapshot.revision != fence.expectedRevision || snapshot.catalogRevision != fence.catalogRevision)
            return Result<void>::Failure(MakeError(PCGDocumentError(Stale)));
        if (fence.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PCGDocumentError(Cancelled)));
        if (!fence.operations.empty())
            return Result<void>::Failure(MakeError(PCGDocumentError(Invalid)));
        if (const auto valid = ValidatePCGCanvas(request.presentation, *snapshot.source); valid.HasError())
            return valid;
        return ExecutePCGEditorRequest(document, request, snapshot);
    }

    /** @copydoc EditorWorkspaceController::ExecutePCGEditorRequest */
    Result<void> EditorWorkspaceController::ExecutePCGEditorRequest(PCGWorkspaceDocument &document, const PCGEditorRequest &request,
                                                                    const PCGDocumentSnapshot &snapshot) {
        using enum PCGEditorAction;
        const auto instance = snapshot.identity.instance;
        if (request.action == ApplyProperties) {
            if (!request.authoring)
                return Result<void>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
            auto prepared = request.authoring->Command(snapshot, m_viewModel.pcgSchemas, request.fence.canEdit);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            auto command = std::move(prepared).Value();
            command.cancellation = request.fence.cancellation;
            return ExecutePCGDocument(instance, command);
        }
        if (request.action == Present) {
            document.canvas = request.presentation;
            return Result<void>::Success();
        }
        if (request.action == PreviewRefresh)
            return RequestPCGPreview(instance, request.fence.cancellation);
        if (request.action == PreviewCancel) {
            if (const auto owner = std::ranges::find(m_pcgPreviews, instance, &PCGPreviewOwner::document); owner != m_pcgPreviews.end())
                static_cast<void>(owner->session->Cancel());
            UpdatePCGPreviews();
            return Result<void>::Success();
        }
        if (request.action == Save) {
            const auto saved = SavePCGDocument(instance, request.fence);
            if (saved.HasError())
                return Result<void>::Failure(saved.ErrorValue());
            if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(*saved.Value().durabilityError);
            return Result<void>::Success();
        }
        if (request.action == Undo || request.action == Redo)
            return ReplayPCGDocument(instance, request.fence, request.action == Redo);
        const auto edit = PreparePCGEdit(request, *snapshot.source, m_viewModel.pcgPalette);
        if (edit.HasError())
            return Result<void>::Failure(edit.ErrorValue());
        const auto executed = ExecutePCGDocument(instance, edit.Value());
        if (executed.HasValue() && request.action == CreateNode) {
            const auto id = std::get<PutPCGNode>(edit.Value().operations.front()).value.id;
            document.canvas.selection = {id};
            const auto placement = std::ranges::find(document.canvas.placements, id, &PCGNodePlacement::node);
            if (placement != document.canvas.placements.end())
                placement->position = {-document.canvas.pan.x / document.canvas.zoom, -document.canvas.pan.y / document.canvas.zoom};
        }
        return executed;
    }

    /** @copydoc EditorWorkspaceController::ProcessPCGDocumentCommand */
    bool EditorWorkspaceController::ProcessPCGDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::EditPCGGraph)
            return ProcessPCGDocumentShortcut(command);
        if (!command.documentInstance || !command.pcgRequest)
            return true;
        auto &documents = m_viewModel.pcgDocuments;
        if (const auto found = std::ranges::find(documents, *command.documentInstance,
                                                 [](const auto &entry) {
            return entry.snapshot.identity.instance;
        });
            found != documents.end()) {
            const auto result = ApplyPCGEditorRequest(*found, *command.pcgRequest);
            found->error = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
        }
        return true;
    }
}  // namespace Horo::Editor
