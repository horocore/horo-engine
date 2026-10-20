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
        const auto existing = std::ranges::find_if(m_prefabDocuments, [&](const auto &entry) {
            return entry->Snapshot().identity.instance == opened.identity.instance;
        });
        if (existing != m_prefabDocuments.end()) {
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
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (!asset || asset->type.Value() != "core.prefab" || m_prefabDocuments.size() >= 16 || limits.HasError()) {
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
        auto tab = m_viewModel.workspacePanelHost.OpenDocument(opened.identity.key);
        if (tab.HasError()) {
            fail(tab.ErrorValue());
            return;
        }
        m_prefabDocuments.push_back(std::move(session).Value());
        m_viewModel.prefabDocumentError.reset();
    }

    /** @copydoc EditorWorkspaceController::RefreshPrefabDocuments */
    void EditorWorkspaceController::RefreshPrefabDocuments() {
        if (m_prefabRefreshing)
            return;

        struct RefreshGuard final {
            bool &active;

            explicit RefreshGuard(bool &value) : active(value) {
                active = true;
            }

            ~RefreshGuard() {
                active = false;
            }
        } guard(m_prefabRefreshing);

        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        auto found = std::ranges::find_if(m_prefabDocuments, [active](const auto &entry) {
            return entry->Snapshot().identity.instance == active;
        });
        const std::optional<DocumentInstanceId> next = found == m_prefabDocuments.end() ? std::nullopt : active;
        const bool switched = next != m_activePrefabDocument;
        for (const auto &entry : m_prefabDocuments)
            if (entry->Snapshot().identity.instance != next)
                entry->ReleaseStage();
        if (switched) {
            m_prefabViewportScene = {};
            m_activePrefabDocument = next;
            m_prefabStageAttemptRevision = 0;
            ++m_viewportSceneRevision;
        }
        if (found != m_prefabDocuments.end()) {
            auto &document = **found;
            const auto snapshot = document.Snapshot();
            if (!document.Stage() && m_prefabStageAttemptRevision != snapshot.fence.revision) {
                m_prefabStageAttemptRevision = snapshot.fence.revision;
                m_prefabViewportScene = {};
                ++m_viewportSceneRevision;
                auto limits = Prefab::PrefabLimitProfile::Create({});
                const std::array roots{snapshot.source->Data().assetId};
                auto captured =
                    m_prefabSourceCapture
                        ? m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), m_prefabCancellation.Token())
                        : Result<PreparedPrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                if (captured.HasError())
                    m_viewModel.prefabDocumentError = captured.ErrorValue();
                else if (m_nextPrefabRuntime == std::numeric_limits<std::uint64_t>::max())
                    m_viewModel.prefabDocumentError = MakeError(Prefab::PrefabErrors::WorkBudgetExceeded);
                else if (auto activated = document.ActivateStage(m_assetRegistry, captured.Value().sources.resolver,
                                                                 Runtime::SceneRuntimeId{m_nextPrefabRuntime++});
                         activated.HasError())
                    m_viewModel.prefabDocumentError = activated.ErrorValue();
                else {
                    auto extracted = ExtractEditorViewportScene(document.Stage()->View(), {snapshot.fence.revision},
                                                                m_viewModel.viewportCamera, m_primitiveMeshCache);
                    if (extracted.HasError()) {
                        document.ReleaseStage();
                        m_viewModel.prefabDocumentError = extracted.ErrorValue();
                    } else
                        m_prefabViewportScene = std::move(extracted).Value();
                }
                m_viewModel.prefabRecoveryAvailable = false;
                m_viewModel.prefabRecovery.reset();
                if (m_mutations && m_durableFiles) {
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
            }
        }
        m_viewModel.prefabDocuments.clear();
        for (const auto &entry : m_prefabDocuments) {
            auto snapshot = entry->Snapshot();
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(snapshot.identity.instance, snapshot.dirty));
            m_viewModel.prefabDocuments.push_back(std::move(snapshot));
        }
    }

    /** @copydoc EditorWorkspaceController::ClosePrefabDocument */
    Result<void> EditorWorkspaceController::ClosePrefabDocument(const DocumentInstanceId instance, const bool discard) {
        auto found = std::ranges::find_if(m_prefabDocuments, [instance](const auto &entry) {
            return entry->Snapshot().identity.instance == instance;
        });
        if (found == m_prefabDocuments.end())
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto snapshot = (*found)->Snapshot();
        if (snapshot.dirty && !discard)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        if (discard && m_mutations && m_durableFiles) {
            DocumentRecoveryService recovery(m_sourceOpenService.ProjectRoot(), *m_mutations, *m_durableFiles);
            auto candidate = recovery.Inspect(snapshot.source->Data().assetId, snapshot.identity.key);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            if (candidate.Value() && candidate.Value()->sessionOwner == snapshot.fence.owner &&
                candidate.Value()->sessionGeneration == snapshot.identity.instance.Value()) {
                if (auto discarded = recovery.Discard(*candidate.Value()); discarded.HasError())
                    return discarded;
            }
        }
        if (auto closed = (*found)->Close(snapshot.fence, discard); closed.HasError())
            return closed;
        auto closedTab = m_viewModel.workspacePanelHost.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        m_prefabDocuments.erase(found);
        return closedTab;
    }

    /** @copydoc EditorWorkspaceController::ProcessPrefabDocumentCommand */
    bool EditorWorkspaceController::ProcessPrefabDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::PrefabDocument)
            return false;
        if (!command.prefabFence)
            return true;
        auto found = std::ranges::find_if(m_prefabDocuments, [&](const auto &entry) {
            return entry->Snapshot().identity.instance == command.prefabFence->instance;
        });
        if (found == m_prefabDocuments.end())
            return true;
        auto &document = **found;
        const auto fence = *command.prefabFence;
        const auto snapshot = document.Snapshot();
        if (fence.owner != snapshot.fence.owner || fence.revision != snapshot.fence.revision) {
            m_viewModel.prefabDocumentError = MakeError(SourceDocumentErrors::Stale);
            return true;
        }
        Result<void> result = Result<void>::Success();
        if (command.prefabEdit)
            result = document.Execute(fence, *command.prefabEdit);
        else
            switch (command.prefabAction) {
                using enum PrefabDocumentAction;
                case Undo:
                    result = document.Replay(fence, false);
                    break;
                case Redo:
                    result = document.Replay(fence, true);
                    break;
                case Reload:
                    result = document.Reload(fence, false);
                    break;
                case DiscardAndReload:
                    result = document.Reload(fence, true);
                    break;
                case DiscardAndClose:
                    result = ClosePrefabDocument(fence.instance, true);
                    break;
                case Save: {
                    if (!m_mutations || !m_durableFiles)
                        result = Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                    else {
                        auto saved = document.Save(fence, *m_mutations, *m_durableFiles);
                        if (saved.HasError())
                            result = Result<void>::Failure(saved.ErrorValue());
                        else {
                            ScheduleContentBrowserPreviews();
                            if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                                result = Result<void>::Failure(*saved.Value().durabilityError);
                        }
                    }
                    break;
                }
                case Recover: {
                    if (!m_mutations || !m_durableFiles)
                        result = Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                    else if (!command.prefabRecovery)
                        result = Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
                    else
                        result = document.Recover(fence, *command.prefabRecovery, *m_mutations, *m_durableFiles);
                    break;
                }
            }
        if (result.HasError())
            m_viewModel.prefabDocumentError = result.ErrorValue();
        else {
            m_viewModel.prefabDocumentError.reset();
            if (!command.prefabEdit &&
                (command.prefabAction == PrefabDocumentAction::Reload || command.prefabAction == PrefabDocumentAction::DiscardAndReload))
                ScheduleContentBrowserPreviews();
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::HasDirtyPrefabDocuments */
    bool EditorWorkspaceController::HasDirtyPrefabDocuments() const {
        return std::ranges::any_of(m_prefabDocuments, [](const auto &entry) {
            return entry->Snapshot().dirty;
        });
    }

    /** @copydoc EditorWorkspaceController::SavePrefabDocuments */
    Result<void> EditorWorkspaceController::SavePrefabDocuments() {
        Result<void> outcome = Result<void>::Success();
        bool savedAny{};
        for (const auto &entry : m_prefabDocuments) {
            const auto snapshot = entry->Snapshot();
            if (!snapshot.dirty)
                continue;
            if (!m_mutations || !m_durableFiles) {
                outcome = Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
                break;
            }
            auto result = entry->Save(snapshot.fence, *m_mutations, *m_durableFiles);
            if (result.HasError()) {
                outcome = Result<void>::Failure(result.ErrorValue());
                break;
            }
            savedAny = true;
            if (result.Value().status != Assets::AssetSourcePublicationStatus::Durable) {
                outcome = Result<void>::Failure(*result.Value().durabilityError);
                break;
            }
        }
        // A partial multi-document save still invalidates every preview of a source already published.
        if (savedAny)
            ScheduleContentBrowserPreviews();
        RefreshPrefabDocuments();
        return outcome;
    }

    /** @copydoc EditorWorkspaceController::UpdatePrefabAutosave */
    void EditorWorkspaceController::UpdatePrefabAutosave(const float seconds, const int minutes) {
        if (!std::isfinite(seconds) || seconds < 0 || minutes < 0)
            return;
        if (minutes > 0) {
            m_prefabAutosaveSeconds += seconds;
            if (m_prefabAutosaveSeconds < static_cast<float>(minutes) * 60)
                return;
        } else if (seconds != 0)
            return;
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
                if (auto result = entry->Close(entry->Snapshot().fence, true); result.HasError())
                    return result;
            }
            m_prefabDocuments.clear();
        } else {
            while (!m_prefabDocuments.empty()) {
                if (auto result = ClosePrefabDocument(m_prefabDocuments.back()->Snapshot().identity.instance, true); result.HasError())
                    return result;
            }
        }
        m_activePrefabDocument.reset();
        m_prefabViewportScene = {};
        m_viewModel.prefabDocuments.clear();
        ++m_viewportSceneRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
