#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::OpenEmbeddedPrefabSource */
    void EditorWorkspaceController::OpenEmbeddedPrefabSource(const SourceOpenResult &result) {
        const auto &opened = *result.document;
        if (const auto existing = std::ranges::find_if(m_prefabDocuments,
                                                       [&](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == opened.identity.instance;
        });
            existing != m_prefabDocuments.end()) {
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(opened.identity.instance));
            return;
        }
        const auto fail = [&](const Error &error) {
            m_viewModel.prefabDocumentError = error;
            m_viewModel.contentBrowserOperationError = "workspace.source_open.unavailable";
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
        };
        const auto *asset = m_assetRegistry.FindByPath(result.location.document.Value());
        auto limits = PrefabLimits();
        if (!asset || asset->type.Value() != "core.prefab" || limits.HasError()) {
            fail(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            return;
        }
        const auto suspended = std::ranges::find_if(m_prefabDocuments, [&](const auto &entry) {
            if (entry->HasPresentation())
                return false;
            const auto snapshot = entry->Snapshot();
            return snapshot.identity.key == opened.identity.key && snapshot.source->Data().assetId == asset->id;
        });
        if (suspended != m_prefabDocuments.end()) {
            auto &source = **suspended;
            if (auto inspected = PrefabApplyTransaction::InspectSource(source); inspected.HasError()) {
                fail(inspected.ErrorValue());
                return;
            }
            if (!source.Snapshot().externalConflict) {
                auto identity = opened.identity;
                if (auto tab = m_viewModel.workspacePanelHost.OpenDocument(opened.identity.key); tab.HasError()) {
                    fail(tab.ErrorValue());
                    return;
                }
                source.ResumePresentation(std::move(identity));
                m_viewModel.prefabDocumentError.reset();
                return;
            }
            ReconcilePrefabApplyHistory();
        }
        if (m_prefabDocuments.size() >= 16) {
            fail(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            return;
        }
        auto session = PrefabDocumentSession::Open(m_sourceOpenService.ProjectRoot(), result.location.absolutePath, asset->id,
                                                   opened.identity, m_workspaceSession, limits.Value());
        if (session.HasError()) {
            fail(session.ErrorValue());
            return;
        }
        // Reserve before publishing the tab so an allocation cannot strand an unowned persistent surface.
        m_prefabDocuments.reserve(m_prefabDocuments.size() + 1);
        if (auto tab = m_viewModel.workspacePanelHost.OpenDocument(opened.identity.key); tab.HasError()) {
            fail(tab.ErrorValue());
            return;
        }
        m_prefabDocuments.push_back(std::move(session).Value());
        m_viewModel.prefabDocumentError.reset();
    }

    /** @copydoc EditorWorkspaceController::PreparePrefabStage */
    void EditorWorkspaceController::PreparePrefabStage(PrefabDocumentSession &document) {
        const auto snapshot = document.Snapshot();
        m_prefabStageAttemptRevision = snapshot.fence.revision;
        m_prefabStageExtractedRevision = 0;
        m_prefabViewportScene = {};
        ++m_viewportSceneRevision;
        auto limits = PrefabLimits();
        if (limits.HasError()) {
            m_viewModel.prefabDocumentError = limits.ErrorValue();
            return;
        }
        const std::array roots{snapshot.source->Data().assetId};
        if (auto captured = m_prefabSourceCapture
                                ? m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), m_prefabCancellation.Token())
                                : Result<PreparedPrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            captured.HasError())
            m_viewModel.prefabDocumentError = captured.ErrorValue();
        else if (m_nextPrefabRuntime > EditorLastPrefabStage.value)
            m_viewModel.prefabDocumentError = MakeError(Prefab::PrefabErrors::WorkBudgetExceeded);
        else if (auto activated = document.ActivateStage(m_assetRegistry, captured.Value().sources.resolver,
                                                         Runtime::SceneRuntimeId{m_nextPrefabRuntime++}, m_prefabStageFactory);
                 activated.HasError())
            m_viewModel.prefabDocumentError = activated.ErrorValue();
        RefreshPrefabRecovery(snapshot);
    }

    /** @copydoc EditorWorkspaceController::RefreshPrefabRecovery */
    void EditorWorkspaceController::RefreshPrefabRecovery(const PrefabDocumentSnapshot &snapshot) {
        m_viewModel.prefabRecoveryAvailable = false;
        m_viewModel.prefabRecovery.reset();
        if (!m_mutations || !m_durableFiles)
            return;
        DocumentRecoveryService recovery(m_sourceOpenService.ProjectRoot(), *m_mutations, *m_durableFiles);
        auto candidate = recovery.Inspect(snapshot.source->Data().assetId, snapshot.identity.key);
        if (candidate.HasError())
            m_viewModel.prefabDocumentError = candidate.ErrorValue();
        else {
            m_viewModel.prefabRecoveryAvailable = candidate.Value().has_value();
            if (candidate.Value())
                m_viewModel.prefabRecovery = std::make_shared<const DocumentRecoveryRecord>(*candidate.Value());
        }
    }

    /** @copydoc EditorWorkspaceController::ExtractPrefabStage */
    void EditorWorkspaceController::ExtractPrefabStage(PrefabDocumentSession &document) {
        if (!document.HasStage())
            return;
        auto advanced = document.AdvanceStage();
        if (advanced.HasError()) {
            m_viewModel.prefabDocumentError = advanced.ErrorValue();
            return;
        }
        if (!advanced.Value()) {
            m_prefabStagePending = true;
            return;
        }
        const auto snapshot = document.Snapshot();
        if (m_prefabStageExtractedRevision == snapshot.previewRevision)
            return;
        auto extracted =
            ExtractEditorViewportScene(*document.StageView(), {snapshot.fence.revision}, m_viewModel.viewportCamera, m_primitiveMeshCache);
        if (extracted.HasError()) {
            document.ReleaseStage();
            m_viewModel.prefabDocumentError = extracted.ErrorValue();
            return;
        }
        m_prefabViewportScene = std::move(extracted).Value();
        m_prefabStageExtractedRevision = snapshot.previewRevision;
        ++m_viewportSceneRevision;
    }

    /** @copydoc EditorWorkspaceController::RefreshPrefabDocuments */
    void EditorWorkspaceController::RefreshPrefabDocuments() {
        if (m_prefabRefreshing)
            return;

        struct RefreshGuard final {
            bool *active;

            explicit RefreshGuard(bool &value) : active(&value) {
                *active = true;
            }

            RefreshGuard(const RefreshGuard &) = delete;
            RefreshGuard &operator=(const RefreshGuard &) = delete;

            ~RefreshGuard() {
                *active = false;
            }
        };

        RefreshGuard guard(m_prefabRefreshing);
        ReconcilePrefabApplyHistory();

        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        auto found = std::ranges::find_if(m_prefabDocuments, [active](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == active;
        });
        const std::optional<DocumentInstanceId> next = found == m_prefabDocuments.end() ? std::nullopt : active;
        for (const auto &entry : m_prefabDocuments)
            if (entry->Snapshot().identity.instance != next)
                entry->ReleaseStage();
        if (next != m_activePrefabDocument) {
            m_prefabViewportScene = {};
            m_activePrefabDocument = next;
            m_prefabStageAttemptRevision = 0;
            m_prefabStageExtractedRevision = 0;
            ++m_viewportSceneRevision;
        }
        m_prefabStagePending = false;
        if (found != m_prefabDocuments.end()) {
            auto &document = **found;
            const auto snapshot = document.Snapshot();
            if (!snapshot.externalConflict && !document.HasStage() && m_prefabStageAttemptRevision != snapshot.fence.revision)
                PreparePrefabStage(document);
            ExtractPrefabStage(document);
        }
        m_viewModel.prefabDocuments.clear();
        for (const auto &entry : m_prefabDocuments) {
            if (!entry->HasPresentation())
                continue;
            auto snapshot = entry->Snapshot();
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(snapshot.identity.instance, snapshot.dirty));
            m_viewModel.prefabDocuments.push_back(std::move(snapshot));
        }
    }

    /** @copydoc EditorWorkspaceController::ClosePrefabDocument */
    Result<void> EditorWorkspaceController::ClosePrefabDocument(const DocumentInstanceId instance, const bool discard) {
        auto found = std::ranges::find_if(m_prefabDocuments, [instance](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == instance;
        });
        if (found == m_prefabDocuments.end())
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto snapshot = (*found)->Snapshot();
        if (snapshot.dirty && !discard)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        if (auto drained = DrainPrefabRecovery(instance); drained.HasError())
            return drained;
        if (discard && m_mutations && m_durableFiles) {
            if (auto discarded = (*found)->DiscardOwnedRecovery(*m_mutations, *m_durableFiles); discarded.HasError())
                return discarded;
        }
        if (!discard && (*found)->HasApplyHistory()) {
            auto closedTab = m_viewModel.workspacePanelHost.CloseDocument(instance);
            if (closedTab.HasValue())
                (*found)->SuspendPresentation();
            return closedTab;
        }
        if (auto closed = (*found)->Close(snapshot.fence, discard); closed.HasError())
            return closed;
        auto closedTab = m_viewModel.workspacePanelHost.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        m_prefabDocuments.erase(found);
        return closedTab;
    }

    /** @copydoc EditorWorkspaceController::ProcessPrefabDocumentCommand */
    bool EditorWorkspaceController::ProcessPrefabDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command == EditorWorkspaceViewCommand::CloseWorkspaceDocument && command.documentInstance &&
            std::ranges::any_of(m_prefabDocuments, [&](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == *command.documentInstance;
        })) {
            if (const auto result = ClosePrefabDocument(*command.documentInstance, false); result.HasError())
                m_viewModel.prefabDocumentError = result.ErrorValue();
            return true;
        }
        if (command.command != EditorWorkspaceViewCommand::PrefabDocument)
            return false;
        if (!command.prefabFence)
            return true;
        auto found = std::ranges::find_if(m_prefabDocuments, [&](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == command.prefabFence->instance;
        });
        if (found == m_prefabDocuments.end())
            return true;
        auto &document = **found;
        const auto fence = *command.prefabFence;
        if (const auto snapshot = document.Snapshot(); fence.owner != snapshot.fence.owner || fence.revision != snapshot.fence.revision) {
            m_viewModel.prefabDocumentError = MakeError(SourceDocumentErrors::Stale);
            return true;
        }
        if (const auto result =
                command.prefabEdit ? document.Execute(fence, *command.prefabEdit) : ApplyPrefabDocumentAction(document, command);
            result.HasError())
            m_viewModel.prefabDocumentError = result.ErrorValue();
        else {
            m_viewModel.prefabDocumentError.reset();
            if (!command.prefabEdit &&
                (command.prefabAction == PrefabDocumentAction::Reload || command.prefabAction == PrefabDocumentAction::DiscardAndReload))
                ScheduleContentBrowserPreviews();
            if (command.prefabAction == PrefabDocumentAction::DiscardRecovery) {
                m_viewModel.prefabRecovery.reset();
                m_viewModel.prefabRecoveryAvailable = false;
            }
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::ApplyPrefabDocumentAction */
    Result<void> EditorWorkspaceController::ApplyPrefabDocumentAction(PrefabDocumentSession &document,
                                                                      const EditorWorkspaceViewCommandData &command) {
        const auto fence = *command.prefabFence;
        if (command.prefabAction != PrefabDocumentAction::KeepExternalDraft)
            if (auto drained = DrainPrefabRecovery(); drained.HasError())
                return drained;
        switch (command.prefabAction) {
            using enum PrefabDocumentAction;
            case Undo:
            case Redo: {
                const bool redo = command.prefabAction == Redo;
                auto paired = ReplayPrefabApply(redo, &document);
                if (paired.HasError())
                    return Result<void>::Failure(std::move(paired).ErrorValue());
                return paired.Value() ? Result<void>::Success() : document.Replay(fence, redo);
            }
            case CompareExternal:
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
            case KeepExternalDraft:
                if (!command.prefabExternalComparison)
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
                return document.KeepExternalDraft(fence, *command.prefabExternalComparison);
            case SaveAs:
                if (!command.prefabSaveDestination)
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
                return SavePrefabAs(document.Snapshot(), *command.prefabSaveDestination);
            case Reload:
                return document.Reload(fence, false);
            case DiscardAndReload:
                return document.Reload(fence, true);
            case DiscardAndClose:
                return ClosePrefabDocument(fence.instance, true);
            case Save: {
                if (!m_mutations || !m_durableFiles)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                auto saved = document.Save(fence, *m_mutations, *m_durableFiles);
                if (saved.HasError())
                    return Result<void>::Failure(saved.ErrorValue());
                ScheduleContentBrowserPreviews();
                return saved.Value().status == Assets::AssetSourcePublicationStatus::Durable
                           ? Result<void>::Success()
                           : Result<void>::Failure(*saved.Value().durabilityError);
            }
            case Recover:
            case DiscardRecovery:
                if (!m_mutations || !m_durableFiles)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                if (!command.prefabRecovery)
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
                return command.prefabAction == Recover
                           ? document.Recover(fence, *command.prefabRecovery, *m_mutations, *m_durableFiles)
                           : document.DiscardRecoveryCandidate(fence, *command.prefabRecovery, *m_mutations, *m_durableFiles);
        }
        return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
    }

    /** @copydoc EditorWorkspaceController::HasDirtyPrefabDocuments */
    bool EditorWorkspaceController::HasDirtyPrefabDocuments() const {
        return std::ranges::any_of(m_prefabDocuments, [](const auto &entry) {
            return entry->Snapshot().dirty;
        });
    }

    /** @copydoc EditorWorkspaceController::SavePrefabDocuments */
    Result<void> EditorWorkspaceController::SavePrefabDocuments() {
        if (auto drained = DrainPrefabRecovery(); drained.HasError())
            return drained;
        UpdatePrefabSettingsProjection();
        Result<void> outcome = Result<void>::Success();
        bool savedAny{};
        const auto save = [this, &savedAny](PrefabDocumentSession &document, const PrefabDocumentFence &fence) {
            if (!m_mutations || !m_durableFiles)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
            auto result = document.Save(fence, *m_mutations, *m_durableFiles);
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            savedAny = true;
            return result.Value().status == Assets::AssetSourcePublicationStatus::Durable
                       ? Result<void>::Success()
                       : Result<void>::Failure(*result.Value().durabilityError);
        };
        for (auto entry = m_prefabDocuments.begin(); entry != m_prefabDocuments.end() && outcome.HasValue(); ++entry) {
            const auto snapshot = (*entry)->Snapshot();
            if (snapshot.dirty)
                outcome = save(**entry, snapshot.fence);
        }
        // A partial multi-document save still invalidates every preview of a source already published.
        if (savedAny)
            ScheduleContentBrowserPreviews();
        RefreshPrefabDocuments();
        return outcome;
    }

    /** @copydoc EditorWorkspaceController::ObservePrefabExternalWatch */
    bool EditorWorkspaceController::ObservePrefabExternalWatch() {
        if (!m_prefabExternalWatch)
            return true;
        const auto status = m_prefabExternalWatch->job.Snapshot();
        if (!status || !status->terminalResult)
            return false;
        auto finished = std::move(*m_prefabExternalWatch);
        m_prefabExternalWatch.reset();
        if (status->state != JobState::Succeeded)
            return true;
        const auto instance = finished.request->fence.instance;
        const auto owner = std::ranges::find_if(m_prefabDocuments, [instance](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == instance;
        });
        if (owner == m_prefabDocuments.end() || !(*owner)->ObserveExternal(*finished.request, *finished.comparison))
            return true;
        const auto snapshot = (*owner)->Snapshot();
        const auto published = std::ranges::find_if(m_viewModel.prefabDocuments, [instance](const auto &entry) {
            return entry.identity.instance == instance;
        });
        if (published != m_viewModel.prefabDocuments.end())
            *published = snapshot;
        if (m_activePrefabDocument == instance) {
            m_prefabViewportScene = {};
            ++m_viewportSceneRevision;
            if (!snapshot.externalConflict)
                m_prefabStageAttemptRevision = 0;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::UpdatePrefabExternalWatch */
    void EditorWorkspaceController::UpdatePrefabExternalWatch(const float seconds) {
        if (!ObservePrefabExternalWatch())
            return;
        if (!m_sourceRecoveryJobs || !m_activePrefabDocument || !std::isfinite(seconds) || seconds <= 0.0F)
            return;
        m_prefabExternalWatchSeconds += seconds;
        if (m_prefabExternalWatchSeconds < 1.0F)
            return;
        m_prefabExternalWatchSeconds = 0;
        const auto owner = std::ranges::find_if(m_prefabDocuments, [this](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == m_activePrefabDocument;
        });
        if (owner == m_prefabDocuments.end() || !(*owner)->Snapshot().dirty)
            return;
        auto request = std::make_shared<const PrefabExternalInspection>((*owner)->CaptureExternalInspection());
        auto comparison = std::make_shared<std::shared_ptr<const PrefabExternalComparison>>();
        auto job = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                      [request, comparison](const CancellationToken &cancellation) {
            auto result = ComparePrefabExternal(*request, cancellation);
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            *comparison = std::move(result).Value();
            return Result<void>::Success();
        });
        if (job.HasValue())
            m_prefabExternalWatch.emplace(PrefabExternalWatchWork{std::move(job).Value(), std::move(request), std::move(comparison)});
    }

    /** @copydoc EditorWorkspaceController::StopPrefabExternalWatch */
    void EditorWorkspaceController::StopPrefabExternalWatch() {
        if (!m_prefabExternalWatch)
            return;
        static_cast<void>(m_prefabExternalWatch->job.RequestCancel());
        static_cast<void>(m_prefabExternalWatch->job.Wait());
        m_prefabExternalWatch.reset();
    }

    /** @copydoc EditorWorkspaceController::ObservePrefabRecovery */
    bool EditorWorkspaceController::ObservePrefabRecovery() {
        if (!m_prefabRecoveryWork)
            return true;
        const auto snapshot = m_prefabRecoveryWork->job.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return false;
        const auto checkpoint = std::move(m_prefabRecoveryWork->checkpoint);
        m_prefabRecoveryWork.reset();
        const auto instance = checkpoint->record.sessionGeneration;
        if (snapshot->state == JobState::Succeeded) {
            const auto owner = std::ranges::find_if(m_prefabDocuments, [instance](const auto &entry) {
                return entry->HasPresentation() && entry->Instance().Value() == instance;
            });
            if (owner != m_prefabDocuments.end()) {
                (*owner)->AcknowledgeRecovery(*checkpoint);
                const auto entry = std::ranges::find_if(m_viewModel.prefabDocuments, [instance](const auto &item) {
                    return item.identity.instance.Value() == instance;
                });
                if (entry != m_viewModel.prefabDocuments.end())
                    entry->recoveryRevision = (*owner)->Snapshot().recoveryRevision;
                if (m_activePrefabDocument == (*owner)->Instance() && checkpoint->acceptedRecovery) {
                    m_viewModel.prefabRecoveryAvailable = false;
                    m_viewModel.prefabRecovery.reset();
                }
            }
        } else if (snapshot->state != JobState::Cancelled && snapshot->terminalResult->error) {
            m_viewModel.prefabDocumentError = snapshot->terminalResult->error;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::DrainPrefabRecovery */
    Result<void> EditorWorkspaceController::DrainPrefabRecovery(const std::optional<DocumentInstanceId> instance) {
        if (!m_prefabRecoveryWork || (instance && m_prefabRecoveryWork->checkpoint->record.sessionGeneration != instance->Value()))
            return Result<void>::Success();
        static_cast<void>(m_prefabRecoveryWork->job.RequestCancel());
        const auto joined = m_prefabRecoveryWork->job.Wait();
        if (!ObservePrefabRecovery())
            return joined;
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::SchedulePrefabRecovery */
    void EditorWorkspaceController::SchedulePrefabRecovery(PrefabDocumentSession &document) {
        auto captured = document.CaptureRecovery();
        if (captured.HasError()) {
            m_viewModel.prefabDocumentError = captured.ErrorValue();
            return;
        }
        if (!captured.Value())
            return;
        auto checkpoint = std::make_shared<const PrefabRecoveryCheckpoint>(std::move(captured).Value().value());
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                            [root = std::filesystem::path(ProjectRoot()), mutations = m_mutations,
                                                             files = m_durableFiles, checkpoint](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files);
            return WritePrefabRecovery(*checkpoint, recovery, cancellation);
        });
        if (submitted.HasError()) {
            m_viewModel.prefabDocumentError = submitted.ErrorValue();
            return;
        }
        m_prefabRecoveryWork.emplace(PrefabRecoveryWork{std::move(submitted).Value(), std::move(checkpoint)});
    }

    /** @copydoc EditorWorkspaceController::UpdatePrefabAutosave */
    void EditorWorkspaceController::UpdatePrefabAutosave(const float seconds, const int minutes) {
        if (!ObservePrefabRecovery() || !m_mutations || !m_durableFiles || !m_sourceRecoveryJobs || minutes <= 0 || seconds < 0 ||
            !std::isfinite(seconds))
            return;
        m_prefabAutosaveSeconds += seconds;
        if (m_prefabRecoveryQueue.empty() && m_prefabAutosaveSeconds >= static_cast<float>(minutes) * 60) {
            m_prefabAutosaveSeconds = 0;
            for (const auto &entry : m_prefabDocuments)
                if (entry->HasPresentation() && entry->Snapshot().dirty)
                    m_prefabRecoveryQueue.push_back(entry->Instance());
        }
        if (m_prefabRecoveryQueue.empty())
            return;
        const auto instance = m_prefabRecoveryQueue.back();
        m_prefabRecoveryQueue.pop_back();
        const auto owner = std::ranges::find_if(m_prefabDocuments, [instance](const auto &entry) {
            return entry->HasPresentation() && entry->Instance() == instance;
        });
        if (owner != m_prefabDocuments.end())
            SchedulePrefabRecovery(**owner);
    }

    /** @copydoc EditorWorkspaceController::FlushPrefabAutosave */
    void EditorWorkspaceController::FlushPrefabAutosave() {
        if (auto drained = DrainPrefabRecovery(); drained.HasError()) {
            m_viewModel.prefabDocumentError = drained.ErrorValue();
            return;
        }
        m_prefabRecoveryQueue.clear();
        m_prefabAutosaveSeconds = 0;
        if (!m_mutations || !m_durableFiles)
            return;
        for (const auto &entry : m_prefabDocuments) {
            if (auto result = entry->Autosave(*m_mutations, *m_durableFiles); result.HasError())
                m_viewModel.prefabDocumentError = result.ErrorValue();
        }
    }

    /** @copydoc EditorWorkspaceController::ShutdownPrefabDocuments */
    Result<void> EditorWorkspaceController::ShutdownPrefabDocuments(const bool discard) {
        StopPrefabExternalWatch();
        if (auto drained = DrainPrefabRecovery(); drained.HasError())
            return drained;
        m_prefabRecoveryQueue.clear();
        if (!discard) {
            for (const auto &entry : m_prefabDocuments) {
                if (!entry->Snapshot().dirty)
                    continue;
                if (!m_mutations || !m_durableFiles)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                if (auto result = entry->Autosave(*m_mutations, *m_durableFiles); result.HasError())
                    return result;
            }
            // Recovery is durable; retire memory without discarding that recovery candidate.
            for (const auto &entry : m_prefabDocuments) {
                if (auto result = entry->Close(entry->Snapshot().fence, true); result.HasError()) {
                    ReconcilePrefabApplyHistory();
                    return result;
                }
            }
            m_prefabDocuments.clear();
        } else {
            while (!m_prefabDocuments.empty()) {
                auto &source = *m_prefabDocuments.back();
                if (source.HasPresentation()) {
                    if (auto result = ClosePrefabDocument(source.Instance(), true); result.HasError()) {
                        ReconcilePrefabApplyHistory();
                        return result;
                    }
                } else {
                    if (auto result = source.Close(source.Snapshot().fence, true); result.HasError()) {
                        ReconcilePrefabApplyHistory();
                        return result;
                    }
                    m_prefabDocuments.pop_back();
                }
            }
        }
        ReconcilePrefabApplyHistory();
        m_activePrefabDocument.reset();
        m_prefabViewportScene = {};
        m_viewModel.prefabDocuments.clear();
        ++m_viewportSceneRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
