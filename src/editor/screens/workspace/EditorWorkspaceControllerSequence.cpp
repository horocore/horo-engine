#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <memory>

namespace Horo::Editor {
    namespace {
        /** @brief Identifies only commands owned by the sequence document presentation boundary. */
        bool IsSequenceCommand(const EditorWorkspaceViewCommand command) noexcept {
            using enum EditorWorkspaceViewCommand;
            return command == FocusSequenceDocument || command == CloseSequenceDocument || command == UpdateSequenceTimeline;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::OpenEmbeddedSequenceSource */
    void EditorWorkspaceController::OpenEmbeddedSequenceSource(const SourceOpenResult &result) {
        const DocumentOpenResult &opened = *result.document;
        auto &sessions = m_viewModel.sequenceDocuments;
        auto loaded = SequenceDocument::Open(opened.identity, result.location.absolutePath);
        if (loaded.HasError()) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = "workspace.sequence.invalid";
            return;
        }
        if (const auto sameAsset = std::ranges::find_if(sessions,
                                                        [&](const SequenceWorkspaceDocument &session) {
            return session.source->identity.instance == opened.identity.instance ||
                   session.source->asset.Data().asset == loaded.Value().asset.Data().asset;
        });
            sameAsset != sessions.end()) {
            if (sameAsset->source->identity.instance != opened.identity.instance)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            if (sameAsset->source->revision != loaded.Value().revision) {
                m_viewModel.contentBrowserOperationError = "workspace.sequence.conflict";
                return;
            }
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(sameAsset->source->identity.instance));
            return;
        }
        // Source leases are load-time state; bound aggregate residency independently of per-file parser limits.
        if (constexpr std::size_t maximumOpenSequences = 32; sessions.size() >= maximumOpenSequences) {
            static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = "workspace.sequence.limit";
            return;
        }
        if (const auto tab = m_viewModel.workspacePanelHost.OpenDocument(opened.identity.key); tab.HasError()) {
            static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
            return;
        }
        sessions.push_back({std::make_shared<const SequenceDocument>(std::move(loaded).Value()), {}});
    }

    /** @copydoc EditorWorkspaceController::OpenEmbeddedAuthoredDocument */
    bool EditorWorkspaceController::OpenEmbeddedAuthoredDocument(const SourceOpenResult &result) {
        if (!result.document || result.route != SourceOpenRoute::EmbeddedWorkspace)
            return false;
        switch (result.classification.kind) {
            case SourceFileKind::Sequence:
                OpenEmbeddedSequenceSource(result);
                return true;
            case SourceFileKind::UiCanvas:
                OpenEmbeddedUiCanvasSource(result);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc EditorWorkspaceController::ProcessSequenceDocumentCommand */
    bool EditorWorkspaceController::ProcessSequenceDocumentCommand(const EditorWorkspaceViewCommandData &cmd) {
        using enum EditorWorkspaceViewCommand;
        if (!IsSequenceCommand(cmd.command))
            return false;
        if (!cmd.documentInstance)
            return true;
        auto &sessions = m_viewModel.sequenceDocuments;
        const auto found = std::ranges::find_if(sessions, [&](const SequenceWorkspaceDocument &session) {
            return session.source->identity.instance == *cmd.documentInstance;
        });
        if (found == sessions.end())
            return true;
        if (cmd.command == FocusSequenceDocument) {
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(*cmd.documentInstance));
        } else if (cmd.command == CloseSequenceDocument) {
            if (!m_viewModel.workspacePanelHost.CloseDocument(*cmd.documentInstance).HasError())
                sessions.erase(found);
        } else if (cmd.sequenceTimeline && m_viewModel.workspacePanelHost.ActiveDocument() == cmd.documentInstance) {
            found->timeline = *cmd.sequenceTimeline;
            found->timeline.Clamp(found->source->asset.Data().durationFrames);
        }
        return true;
    }
}  // namespace Horo::Editor
