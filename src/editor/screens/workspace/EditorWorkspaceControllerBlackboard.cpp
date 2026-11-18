#include "Horo/Assets/AssetSourcePublication.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::FindBlackboardDocument */
    BlackboardSchemaDocument *EditorWorkspaceController::FindBlackboardDocument(DocumentInstanceId instance) noexcept {
        const auto found = std::ranges::find_if(m_blackboardDocuments, [instance](const auto &document) {
            return document->Identity().instance == instance;
        });
        return found == m_blackboardDocuments.end() ? nullptr : found->get();
    }

    /** @copydoc EditorWorkspaceController::OpenEmbeddedBlackboardSource */
    void EditorWorkspaceController::OpenEmbeddedBlackboardSource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        auto &host = m_viewModel.workspacePanelHost;
        if (FindBlackboardDocument(opened.identity.instance)) {
            static_cast<void>(host.FocusDocument(opened.identity.instance));
            return;
        }
        const auto fail = [&](const char *key) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = key;
        };
        constexpr std::size_t MaximumOpenDocuments = 16;
        if (m_blackboardDocuments.size() >= MaximumOpenDocuments) {
            fail("workspace.blackboard.limit");
            return;
        }
        auto loaded = BlackboardSchemaDocument::OpenFile(opened.identity, m_assetRegistry, m_sourceOpenService.ProjectRoot(),
                                                         result.location.absolutePath);
        if (loaded.HasError()) {
            fail("workspace.blackboard.invalid");
            return;
        }
        auto snapshot = loaded.Value()->Snapshot();
        if (snapshot.HasError()) {
            fail("workspace.blackboard.invalid");
            return;
        }
        auto &views = m_viewModel.blackboardDocuments;
        const auto sameAsset = std::ranges::find_if(views, [&](const auto &entry) {
            return entry.snapshot.source->asset == snapshot.Value().source->asset;
        });
        if (sameAsset != views.end()) {
            fail("workspace.blackboard.invalid");
            return;
        }
        if (m_blackboardDocuments.empty()) {
            m_blackboardDocuments.reserve(MaximumOpenDocuments);
            views.reserve(MaximumOpenDocuments);
        }
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        m_blackboardDocuments.push_back(std::move(loaded).Value());
        views.push_back({std::move(snapshot).Value(), {}});
    }

    /** @copydoc EditorWorkspaceController::SaveBlackboardDocument */
    Result<void> EditorWorkspaceController::SaveBlackboardDocument(BlackboardSchemaDocument &document, const std::uint64_t revision) {
        if (!m_mutations)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        NativeDurableFileSystem fallback;
        auto &files = m_durableFiles ? *m_durableFiles : fallback;
        return document.Save(revision, m_assetRegistry, *m_mutations, files);
    }

    /** @copydoc EditorWorkspaceController::RefreshBlackboardDocument */
    void EditorWorkspaceController::RefreshBlackboardDocument(BlackboardSchemaDocument &document, const Result<void> &result) {
        const auto found = std::ranges::find_if(m_viewModel.blackboardDocuments, [&](const auto &entry) {
            return entry.snapshot.document == document.Identity();
        });
        if (found == m_viewModel.blackboardDocuments.end())
            return;
        auto snapshot = document.Snapshot();
        if (snapshot.HasValue()) {
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.Identity().instance, snapshot.Value().dirty));
            found->snapshot = std::move(snapshot).Value();
        }
        found->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
    }

    /** @copydoc EditorWorkspaceController::ProcessBlackboardDocumentCommand */
    bool EditorWorkspaceController::ProcessBlackboardDocumentCommand(const EditorWorkspaceViewCommandData &cmd) {
        if (cmd.command != EditorWorkspaceViewCommand::BlackboardDocument)
            return false;
        if (!cmd.blackboard || m_viewModel.workspacePanelHost.ActiveDocument() != cmd.blackboard->fence.instance)
            return true;
        const auto &request = *cmd.blackboard;
        auto *document = FindBlackboardDocument(request.fence.instance);
        if (!document)
            return true;
        const auto execute = [&]() -> Result<void> {
            using enum BlackboardDocumentOperation;
            switch (request.operation) {
                case Edit:
                    if (request.edit)
                        return document->Execute(request.fence.revision, *request.edit);
                    break;
                case Undo:
                    return document->Undo(request.fence.revision);
                case Redo:
                    return document->Redo(request.fence.revision);
                case Save:
                    return SaveBlackboardDocument(*document, request.fence.revision);
                case Reload:
                    return document->Reload(request.fence.revision, m_assetRegistry);
            }
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        };
        RefreshBlackboardDocument(*document, execute());
        return true;
    }

    /** @copydoc EditorWorkspaceController::ResolveBlackboardClose */
    Result<bool> EditorWorkspaceController::ResolveBlackboardClose(BlackboardDocumentFence fence, SourceCloseDecision decision) {
        if (decision == SourceCloseDecision::Cancel)
            return Result<bool>::Success(true);
        if (decision != SourceCloseDecision::Discard && decision != SourceCloseDecision::Save)
            return Result<bool>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        auto *document = FindBlackboardDocument(fence.instance);
        if (!document)
            return Result<bool>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        const auto current = document->Snapshot();
        if (current.HasError())
            return Result<bool>::Failure(current.ErrorValue());
        if (current.Value().revision != fence.revision)
            return Result<bool>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        if (decision == SourceCloseDecision::Save && current.Value().dirty) {
            const auto saved = SaveBlackboardDocument(*document, fence.revision);
            RefreshBlackboardDocument(*document, saved);
            if (saved.HasError())
                return Result<bool>::Failure(saved.ErrorValue());
        }
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Result<bool>::Failure(closed.ErrorValue());
        std::erase_if(m_viewModel.blackboardDocuments, [fence](const auto &entry) {
            return entry.snapshot.document.instance == fence.instance;
        });
        std::erase_if(m_blackboardDocuments, [document](const auto &owner) {
            return owner.get() == document;
        });
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Editor
