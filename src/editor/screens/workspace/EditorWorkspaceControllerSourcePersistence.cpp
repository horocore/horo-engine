#include "Horo/Foundation/Platform.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::HasDirtySourceDocuments */
    bool EditorWorkspaceController::HasDirtySourceDocuments() {
        RefreshSourceDocumentProjection();
        return std::ranges::any_of(m_viewModel.workspacePanelHost.DocumentTabs(), [](const WorkspaceDocumentTab &tab) {
            return tab.identity.key.kind == DocumentKind::Source && tab.dirty;
        });
    }

    /** @copydoc EditorWorkspaceController::SaveSourceDocuments */
    Result<void> EditorWorkspaceController::SaveSourceDocuments() {
        NativeDurableFileSystem files;
        m_sourceSaveAllOutcome = m_sourceOpenService.Documents().SaveAll(files);
        RefreshSourceDocumentProjection();
        if (m_sourceSaveAllOutcome->HasError())
            return Result<void>::Failure(m_sourceSaveAllOutcome->ErrorValue());
        for (const auto &item : m_sourceSaveAllOutcome->Value()) {
            if (item.result.HasError())
                return Result<void>::Failure(item.result.ErrorValue());
            if (item.result.Value().disposition != SourceSaveDisposition::Durable)
                return Result<void>::Failure(*item.result.Value().diagnostic);
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::RefreshSourceDocumentProjection */
    void EditorWorkspaceController::RefreshSourceDocumentProjection() {
        auto &host = m_viewModel.workspacePanelHost;
        host.RefreshDocuments();
        auto &sources = m_viewModel.sourceDocuments;
        std::size_t index = 0;
        for (const auto &tab : host.DocumentTabs()) {
            if (tab.identity.key.kind != DocumentKind::Source)
                continue;
            if (const auto source = m_sourceOpenService.Documents().Snapshot(tab.identity.instance); source.HasValue()) {
                const auto first = sources.begin() + static_cast<std::ptrdiff_t>(index);
                const auto cached = std::find_if(first, sources.end(), [&](const SourceDocumentSnapshot &candidate) {
                    return candidate.SharesStateWith(source.Value());
                });
                if (cached != sources.end()) {
                    // Reorder leases without recomputing byte-derived dirty state for unchanged publications.
                    std::rotate(first, cached, cached + 1);
                } else {
                    static_cast<void>(host.SetDocumentDirty(tab.identity.instance, source.Value().Dirty()));
                    if (index < sources.size() && sources[index].Identity().instance == tab.identity.instance)
                        sources[index] = source.Value();
                    else
                        sources.insert(first, source.Value());
                }
            } else {
                if (index < sources.size())
                    sources[index] = {};
                else
                    sources.emplace_back();
            }
            ++index;
        }
        sources.resize(index);
    }

    /** @copydoc EditorWorkspaceController::ProcessWorkspaceDocumentCommand */
    bool EditorWorkspaceController::ProcessWorkspaceDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command != FocusWorkspaceDocument && command.command != CloseWorkspaceDocument &&
            command.command != MoveWorkspaceDocument)
            return false;
        if (!command.documentInstance)
            return true;
        auto &host = m_viewModel.workspacePanelHost;
        const auto instance = *command.documentInstance;
        if (command.command == FocusWorkspaceDocument) {
            static_cast<void>(host.FocusDocument(instance));
        } else if (command.command == MoveWorkspaceDocument) {
            static_cast<void>(host.MoveDocument(instance, command.documentMoveBefore));
        } else if (const auto source = m_sourceOpenService.Documents().Snapshot(instance); source.HasValue()) {
            if (source.Value().Dirty()) {
                m_sourceCloseOutcome = Result<bool>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
            } else {
                NativeDurableFileSystem files;
                m_sourceCloseOutcome =
                    m_sourceOpenService.ResolveClose({instance, source.Value().Revision(), {}}, SourceCloseDecision::Save, files);
            }
        } else if (const auto sequence = std::ranges::find_if(m_viewModel.sequenceDocuments,
                                                              [instance](const auto &entry) {
            return entry.source->identity.instance == instance;
        });
                   sequence != m_viewModel.sequenceDocuments.end()) {
            EditorWorkspaceViewCommandData close;
            close.command = CloseSequenceDocument;
            close.documentInstance = instance;
            static_cast<void>(ProcessSequenceDocumentCommand(close));
        } else if (m_pcgDocuments.Snapshot(instance).HasValue()) {
            static_cast<void>(ClosePCGDocument(instance, false));
        } else if (const auto canvas = std::ranges::find_if(m_uiCanvasDocuments,
                                                            [instance](const auto &entry) {
            return entry.Identity().instance == instance;
        });
                   canvas != m_uiCanvasDocuments.end()) {
            if (canvas->Close().HasValue() && host.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges).HasValue())
                m_uiCanvasDocuments.erase(canvas);
        } else {
            // Navigation's owner processes its request at the existing settings safe point.
            for (auto &tab : m_viewModel.navigationDocuments)
                if (tab.identity.instance == instance)
                    tab.editor->requestClose = true;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::ProcessSourceDocumentCommand */
    bool EditorWorkspaceController::ProcessSourceDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        NativeDurableFileSystem files;
        switch (command.command) {
            case EditorWorkspaceViewCommand::SaveSourceDocument:
                if (command.sourceSave)
                    m_sourceSaveOutcome = m_sourceOpenService.Documents().Save(*command.sourceSave, files);
                else
                    m_sourceSaveOutcome = Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveSourceDocumentAs:
                if (command.sourceSave && command.stringPayload)
                    m_sourceSaveOutcome =
                        m_sourceOpenService.SaveAs(*command.sourceSave, std::filesystem::path{*command.stringPayload}, files);
                else
                    m_sourceSaveOutcome = Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveAllSourceDocuments:
                m_sourceSaveAllOutcome = m_sourceOpenService.Documents().SaveAll(files);
                return true;
            case EditorWorkspaceViewCommand::CloseSourceDocument:
                if (command.sourceSave)
                    m_sourceCloseOutcome = m_sourceOpenService.ResolveClose(*command.sourceSave, command.sourceCloseDecision, files);
                else
                    m_sourceCloseOutcome = Result<bool>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            default:
                return false;
        }
    }
}  // namespace Horo::Editor
