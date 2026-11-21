#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Platform.h"
#include "WorkspacePathText.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::UpdateSourceAutosave */
    void EditorWorkspaceController::UpdateSourceAutosave(const float elapsedSeconds, const int delaySeconds) {
        m_sourceReviews.UpdateAuditHistory(m_sourceRecoveryJobs, ProjectRoot(), m_mutations, m_durableFiles);
        if (m_sourceReviews.Poll(m_sourceOpenService.Documents(), m_durableFiles, &m_sourceOpenService, &m_viewModel.workspacePanelHost)) {
            m_sourceReviews.RecordAppliedAudit(m_sourceRecoveryJobs, ProjectRoot(), m_mutations, m_durableFiles);
            RefreshSourceDocumentProjection();
        }
        NativeDurableFileSystem files;
        if (auto outcome = m_sourceOpenService.Documents().UpdateAutosave(elapsedSeconds, delaySeconds, files)) {
            m_sourceSaveOutcome = std::move(*outcome);
            if (m_sourceSaveOutcome->HasError())
                LOG_ERROR("editor.source.autosave", "%s", m_sourceSaveOutcome->ErrorValue().message.c_str());
            RefreshSourceDocumentProjection();
        }
    }

    /** @copydoc EditorWorkspaceController::HasDirtySourceDocuments */
    bool EditorWorkspaceController::HasDirtySourceDocuments() {
        RefreshSourceDocumentProjection();
        RefreshTerrainDocuments();
        for (const auto &[instance, revision] : TerrainDocumentRevisions()) {
            static_cast<void>(revision);
            for (const auto &tab : m_viewModel.workspacePanelHost.DocumentTabs())
                if (tab.identity.instance == instance && tab.dirty)
                    return true;
        }
        return std::ranges::any_of(m_viewModel.workspacePanelHost.DocumentTabs(), [](const WorkspaceDocumentTab &tab) {
            const auto kind = tab.identity.key.kind;
            return tab.dirty && (kind == DocumentKind::Source || kind == DocumentKind::UiCanvas || kind == DocumentKind::BlackboardSchema ||
                                 kind == DocumentKind::DecisionGraph || kind == DocumentKind::EnvironmentQuery);
        });
    }

    /** @copydoc EditorWorkspaceController::SaveSourceDocuments */
    Result<void> EditorWorkspaceController::SaveSourceDocuments() {
        if (auto drained = DrainPrefabRecovery(); drained.HasError())
            return drained;
        if (auto drained = DrainUiCanvasRecovery(); drained.HasError())
            return drained;
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
        for (auto &canvas : m_uiCanvasDocuments) {
            if (!canvas.IsDirty())
                continue;
            if (auto saved = SaveUiCanvasSource(canvas); saved.HasError())
                return saved;
        }
        RefreshUiCanvasProjection();
        for (const auto &document : m_blackboardDocuments) {
            const auto snapshot = document->Snapshot();
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            if (!snapshot.Value().dirty)
                continue;
            const auto saved = SaveBlackboardDocument(*document, snapshot.Value().revision);
            RefreshBlackboardDocument(*document, saved);
            if (saved.HasError())
                return saved;
        }
        if (auto saved = SaveDecisionGraphDocuments(); saved.HasError())
            return saved;
        if (auto saved = SaveEnvironmentQueryDocuments(); saved.HasError())
            return saved;
        return SaveTerrainDocuments();
    }

    /** @copydoc EditorWorkspaceController::RefreshSourceDocumentProjection */
    void EditorWorkspaceController::RefreshSourceDocumentProjection() {
        auto &host = m_viewModel.workspacePanelHost;
        host.RefreshDocuments();
        RefreshUiCanvasProjection();
        auto &sources = m_viewModel.sourceDocuments;
        std::size_t index = 0;
        for (const auto &tab : host.DocumentTabs()) {
            if (tab.identity.key.kind != DocumentKind::Source)
                continue;
            const auto slot = index++;
            const auto source = m_sourceOpenService.Documents().Snapshot(tab.identity.instance);
            if (source.HasError()) {
                if (slot < sources.size())
                    sources[slot] = {};
                else
                    sources.emplace_back();
                continue;
            }
            const auto first = sources.begin() + static_cast<std::ptrdiff_t>(slot);
            if (const auto cached = std::find_if(first, sources.end(),
                                                 [&](const SourceDocumentSnapshot &candidate) {
                return candidate.SharesStateWith(source.Value());
            });
                cached != sources.end()) {
                // Reorder leases without recomputing byte-derived dirty state for unchanged publications.
                std::rotate(first, cached, cached + 1);
                continue;
            }
            static_cast<void>(host.SetDocumentDirty(tab.identity.instance, source.Value().Dirty()));
            if (slot < sources.size() && sources[slot].Identity().instance == tab.identity.instance)
                sources[slot] = source.Value();
            else
                sources.insert(first, source.Value());
        }
        sources.resize(index);
        auto &history = m_viewModel.sourceHistory;
        history.clear();
        history.reserve(sources.size());
        for (const auto &tab : host.DocumentTabs()) {
            if (tab.identity.key.kind != DocumentKind::Source)
                continue;
            const auto state = m_sourceOpenService.Documents().History(tab.identity.instance);
            history.push_back(state.HasValue() ? state.Value() : SourceHistoryState{});
        }
    }

    /** @copydoc EditorWorkspaceController::ResolveUiCanvasClose */
    Result<bool> EditorWorkspaceController::ResolveUiCanvasClose(const UiCanvasDocumentStateId state, const SourceCloseDecision decision) {
        if (decision == SourceCloseDecision::Cancel)
            return Result<bool>::Success(true);
        if (decision != SourceCloseDecision::Save && decision != SourceCloseDecision::Discard)
            return Result<bool>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
        const auto canvas = std::ranges::find_if(m_uiCanvasDocuments, [state](const auto &entry) {
            return entry.Identity().instance == state.instance;
        });
        if (canvas == m_uiCanvasDocuments.end() || canvas->CurrentState() != state)
            return Result<bool>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        if (auto drained = DrainUiCanvasRecovery(); drained.HasError())
            return Result<bool>::Failure(drained.ErrorValue());
        if (decision == SourceCloseDecision::Save && canvas->IsDirty()) {
            if (auto saved = SaveUiCanvasSource(*canvas); saved.HasError())
                return Result<bool>::Failure(saved.ErrorValue());
        }
        if ((decision == SourceCloseDecision::Discard || (!canvas->IsDirty() && canvas->HasRecoveryCheckpoint())) && m_mutations &&
            m_durableFiles) {
            DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
            if (auto discarded = canvas->DiscardRecovery(m_workspaceSession, recovery); discarded.HasError())
                return Result<bool>::Failure(discarded.ErrorValue());
        }
        auto &host = m_viewModel.workspacePanelHost;
        if (const auto tab = host.CloseDocument(state.instance, WorkspaceDocumentClosePolicy::DiscardChanges); tab.HasError())
            return Result<bool>::Failure(tab.ErrorValue());
        static_cast<void>(canvas->Close(UiCanvasClosePolicy::DiscardChanges));
        m_uiCanvasDocuments.erase(canvas);
        RefreshUiCanvasProjection();
        return Result<bool>::Success(true);
    }

    /** @copydoc EditorWorkspaceController::CloseUiCanvasSource */
    void EditorWorkspaceController::CloseUiCanvasSource(const DocumentInstanceId instance) {
        const auto canvas = std::ranges::find_if(m_uiCanvasDocuments, [instance](const auto &entry) {
            return entry.Identity().instance == instance;
        });
        if (canvas == m_uiCanvasDocuments.end())
            return;
        // Presentation offers explicit consent for dirty documents. A bare toolbar/tab command never discards data.
        const auto outcome = canvas->IsDirty() ? Result<bool>::Failure(MakeError(UiCanvasDocumentErrors::DirtyDocument))
                                               : ResolveUiCanvasClose(canvas->CurrentState(), SourceCloseDecision::Save);
        if (outcome.HasError()) {
            const auto projection = std::ranges::find_if(m_viewModel.uiCanvasDocuments, [instance](const auto &entry) {
                return entry.identity.instance == instance;
            });
            if (projection != m_viewModel.uiCanvasDocuments.end())
                projection->diagnostic = outcome.ErrorValue();
        }
    }

    /** @copydoc EditorWorkspaceController::UiCanvasStates */
    std::vector<UiCanvasDocumentStateId> EditorWorkspaceController::UiCanvasStates() const {
        std::vector<UiCanvasDocumentStateId> states;
        states.reserve(m_uiCanvasDocuments.size());
        for (const auto &canvas : m_uiCanvasDocuments)
            states.push_back(canvas.CurrentState());
        return states;
    }

    /** @copydoc EditorWorkspaceController::ShutdownUiCanvasDocuments */
    Result<void> EditorWorkspaceController::ShutdownUiCanvasDocuments(const bool discard) {
        if (auto drained = DrainUiCanvasRecovery(); drained.HasError())
            return drained;
        m_uiCanvasRecoveryQueue.clear();
        if (!discard) {
            for (auto &canvas : m_uiCanvasDocuments) {
                if (!canvas.IsDirty() && !canvas.HasRecoveryCheckpoint())
                    continue;
                if (!m_mutations || !m_durableFiles)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
                if (auto written = canvas.Autosave(m_workspaceSession, recovery); written.HasError())
                    return written;
            }
        }
        while (!m_uiCanvasDocuments.empty()) {
            const auto state = m_uiCanvasDocuments.back().CurrentState();
            if (discard) {
                if (auto closed = ResolveUiCanvasClose(state, SourceCloseDecision::Discard); closed.HasError())
                    return Result<void>::Failure(closed.ErrorValue());
                continue;
            }
            auto &host = m_viewModel.workspacePanelHost;
            if (auto closed = host.CloseDocument(state.instance, WorkspaceDocumentClosePolicy::DiscardChanges); closed.HasError())
                return closed;
            static_cast<void>(m_uiCanvasDocuments.back().Close(UiCanvasClosePolicy::DiscardChanges));
            m_uiCanvasDocuments.pop_back();
        }
        RefreshUiCanvasProjection();
        return Result<void>::Success();
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
        } else if (CloseEnvironmentQuerySource(instance) || CloseDecisionGraphSource(instance)) {
            return true;
        } else if (auto *blackboard = FindBlackboardDocument(instance)) {
            const auto snapshot = blackboard->Snapshot();
            if (snapshot.HasValue()) {
                if (snapshot.Value().dirty)
                    RefreshBlackboardDocument(*blackboard, Result<void>::Failure(MakeError(BlackboardDocumentErrors::Dirty)));
                else
                    static_cast<void>(ResolveBlackboardClose({instance, snapshot.Value().revision}, SourceCloseDecision::Save));
            }
        } else if (m_pcgDocuments.Snapshot(instance).HasValue()) {
            static_cast<void>(ClosePCGDocument(instance, false));
        } else if (const auto canvas = std::ranges::find_if(m_uiCanvasDocuments,
                                                            [instance](const auto &entry) {
            return entry.Identity().instance == instance;
        });
                   canvas != m_uiCanvasDocuments.end()) {
            CloseUiCanvasSource(instance);
        } else {
            // Navigation's owner processes its request at the existing settings safe point.
            for (const auto &tab : m_viewModel.navigationDocuments)
                if (tab.identity.instance == instance)
                    tab.editor->requestClose = true;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::ProcessSourceDocumentCommand */
    bool EditorWorkspaceController::ProcessSourceDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        if (m_sourceReviews.Process(command, m_sourceOpenService.Documents(), m_sourceRecoveryJobs, ProjectRoot(), m_durableFiles,
                                    &m_sourceOpenService)) {
            if (command.command == EditorWorkspaceViewCommand::UndoSourceReview)
                RefreshSourceDocumentProjection();
            return true;
        }
        NativeDurableFileSystem files;
        switch (command.command) {
            case EditorWorkspaceViewCommand::SourceDocumentInteraction:
                m_viewModel.sourceEditingResult =
                    command.sourceInteraction ? SubmitSourceEditingCommand(m_sourceOpenService.Documents(), *command.sourceInteraction)
                                              : Result<SourceEditingPublication>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveSourceDocument:
                if (command.sourceSave)
                    m_sourceSaveOutcome = m_sourceOpenService.Documents().Save(*command.sourceSave, files);
                else
                    m_sourceSaveOutcome = Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return true;
            case EditorWorkspaceViewCommand::SaveSourceDocumentAs:
                if (command.sourceSave && command.stringPayload)
                    m_sourceSaveOutcome =
                        m_sourceOpenService.SaveAs(*command.sourceSave, WorkspaceNativePath(*command.stringPayload), files);
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
