#include "AuthoredAiSourceInventory.h"
#include "EnvironmentQueryWorkspaceSource.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <new>
#include <set>

namespace Horo::Editor {
    namespace EnvironmentQueryWorkspaceSource {
        using AI::EnvironmentQuerySourceCodec;

        /** @copydoc CaptureQuerySource */
        Result<Assets::AssetCookInputSnapshot> CaptureQuerySource(const std::filesystem::path &root,
                                                                  const Assets::AssetRegistrySnapshot &registry, Assets::AssetId source) {
            const Assets::AssetCookLimits limits{.maximumSourceBytes = EnvironmentQuerySourceCodec::MaximumBytes, .maximumAssets = 1};
            return Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&source, 1}, limits,
                                                                   EnvironmentQuerySourceCodec::MaximumBytes);
        }

        /** @copydoc CaptureQuerySidecar */
        Result<Assets::AssetSourceIdentity> CaptureQuerySidecar(const std::filesystem::path &root, const Assets::AssetRecord &record) {
            constexpr std::size_t MaximumIdentitySidecarBytes = 1U << 20U;
            const auto captured =
                Assets::CaptureAssetSource(root, Foundation::Paths::Resolve(root, record.metadataPath), MaximumIdentitySidecarBytes);
            if (captured.HasError())
                return Result<Assets::AssetSourceIdentity>::Failure(captured.ErrorValue());
            return Result<Assets::AssetSourceIdentity>::Success(captured.Value().identity);
        }

        /** @copydoc RegisteredQueryDocument */
        bool RegisteredQueryDocument(const EnvironmentQueryWorkspaceDocument &document, const Assets::AssetRegistrySnapshot &registry) {
            const auto *record = registry.Find(document.asset);
            return record && record->type.Value() == AI::EnvironmentQuerySourceAssetType &&
                   record->sourcePath.String() == document.identity.key.source.Value();
        }

        /** @copydoc DescriptorAvailability */
        QueryDescriptorAvailability DescriptorAvailability(const AI::EnvironmentQueryAsset &query,
                                                           const AI::QuerySchemaRegistry *registry) {
            if (!registry)
                return QueryDescriptorAvailability::RegistryUnavailable;
            return AI::EnvironmentQueryPlan::Compile(query, *registry).HasValue() ? QueryDescriptorAvailability::Validated
                                                                                  : QueryDescriptorAvailability::Incompatible;
        }
    }  // namespace EnvironmentQueryWorkspaceSource

    namespace {
        using AI::EnvironmentQuerySourceCodec;
        using EnvironmentQueryWorkspaceSource::CaptureQuerySidecar;
        using EnvironmentQueryWorkspaceSource::CaptureQuerySource;
        using EnvironmentQueryWorkspaceSource::DescriptorAvailability;
        using EnvironmentQueryWorkspaceSource::RegisteredQueryDocument;

        /** @brief Applies one typed structural edit to a detached, already validated query. */
        Result<AI::EnvironmentQueryAsset> ChangeDistanceStages(const AI::EnvironmentQueryAsset &source,
                                                               const EnvironmentQueryDistanceStageChange &change) {
            if (const auto *remove = std::get_if<AI::QueryStageId>(&change))
                return AI::RemoveBuiltinPointDistanceTest(source, *remove);
            std::set<AI::QueryStageId> occupied;
            for (const auto &stage : source.Stages())
                occupied.insert(stage.id);
            const auto id = AuthoredAiSources::Issue(occupied);
            if (id.HasError())
                return Result<AI::EnvironmentQueryAsset>::Failure(id.ErrorValue());
            return AI::AddBuiltinPointDistanceTest(source, id.Value(), std::get<AI::QueryPointDistanceTest>(change));
        }

        /** @brief Source and settings captured together before preparing the document history entry. */
        struct QueryDraftEdit final {
            AI::EnvironmentQueryAsset source;
            AI::BuiltinPointQuerySettings settings;
        };

        /** @brief Validates value edits and a possible structural change as a single detached transaction. */
        Result<QueryDraftEdit> PrepareQueryDraftEdit(const AI::EnvironmentQueryAsset &source, const AI::QuerySchemaRegistry &registry,
                                                     const AI::BuiltinPointQuerySettings &settings,
                                                     const EnvironmentQueryDistanceStageChange &change) {
            using Prepared = Result<QueryDraftEdit>;
            auto edited = AI::EditBuiltinPointQuerySettings(source, registry, settings);
            if (edited.HasError())
                return Prepared::Failure(edited.ErrorValue());
            if (!std::holds_alternative<std::monostate>(change)) {
                edited = ChangeDistanceStages(edited.Value(), change);
                if (edited.HasError())
                    return Prepared::Failure(edited.ErrorValue());
            }
            auto projected = AI::ReadBuiltinPointQuerySettings(edited.Value(), registry);
            if (projected.HasError())
                return Prepared::Failure(projected.ErrorValue());
            return Prepared::Success({std::move(edited).Value(), std::move(projected).Value()});
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource */
    void EditorWorkspaceController::OpenEmbeddedEnvironmentQuerySource(const SourceOpenResult &result, const bool reloadExternal) {
        const auto &opened = *result.document;
        const auto fail = [&](std::string_view key) {
            if (opened.disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.identity.instance));
            m_viewModel.contentBrowserOperationError = key;
        };
        const auto *record = m_assetRegistry.FindByPath(opened.identity.key.source.Value());
        if (!record || record->type.Value() != AI::EnvironmentQuerySourceAssetType) {
            fail("workspace.environment_query.invalid");
            return;
        }
        auto &documents = m_viewModel.environmentQueryDocuments;
        constexpr std::size_t MaximumDocuments = 16;
        if (documents.empty())
            documents.reserve(MaximumDocuments);
        const auto existing = std::ranges::find_if(documents, [&](const auto &document) {
            return document.identity.instance == opened.identity.instance || document.asset == record->id;
        });
        if (existing == documents.end() && documents.size() >= MaximumDocuments) {
            fail("workspace.environment_query.limit");
            return;
        }
        const auto captured = CaptureQuerySource(ProjectRoot(), m_assetRegistry, record->id);
        if (captured.HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        const auto &source = captured.Value().Sources().front();
        if (!reloadExternal && existing != documents.end() && existing->dirty) {
            if (existing->identity != opened.identity || existing->source != source.sourceDigest) {
                existing->conflict = true;
                existing->diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
                if (existing->identity != opened.identity) {
                    fail("workspace.source_open.unavailable");
                    return;
                }
            }
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(existing->identity.instance));
            return;
        }
        if (!reloadExternal && existing != documents.end() && existing->identity == opened.identity && existing->asset == record->id &&
            existing->source == source.sourceDigest) {
            static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(opened.identity.instance));
            return;
        }
        auto query = EnvironmentQuerySourceCodec::Decode(source.bytes);
        if (query.HasError() || captured.Value().VerifyUnchanged().HasError()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        EnvironmentQueryWorkspaceDocument candidate{opened.identity, record->id, source.sourceDigest,
                                                    std::make_shared<const AI::EnvironmentQueryAsset>(std::move(query).Value())};
        candidate.descriptors = DescriptorAvailability(*candidate.query, m_querySchema);
        decltype(m_environmentQueryDrafts) prepared;
        if (m_querySchema && m_mutations && m_durableFiles) {
            const auto settings = AI::ReadBuiltinPointQuerySettings(*candidate.query, *m_querySchema);
            const auto bytes = EnvironmentQuerySourceCodec::Encode(*candidate.query);
            const auto sidecar = CaptureQuerySidecar(ProjectRoot(), *record);
            if (settings.HasValue() && bytes.HasValue() && sidecar.HasValue() && sidecar.Value().exists &&
                captured.Value().VerifyUnchanged().HasValue()) {
                const auto digest = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
                candidate.settings = settings.Value();
                const EnvironmentQueryState baseline{candidate.query, settings.Value(), digest, bytes.Value().size()};
                prepared.try_emplace(candidate.identity.instance,
                                     EnvironmentQueryDraft{baseline, candidate.query, digest, sidecar.Value().digest, source.bytes.size()});
            }
        }
        if (reloadExternal && prepared.empty()) {
            fail("workspace.environment_query.invalid");
            return;
        }
        if (existing != documents.end()) {
            if (existing->revision == std::numeric_limits<std::uint64_t>::max()) {
                fail("workspace.source_open.unavailable");
                return;
            }
            candidate.revision = existing->revision + 1;
        }
        auto &host = m_viewModel.workspacePanelHost;
        if (host.OpenDocument(opened.identity.key).HasError()) {
            fail("workspace.source_open.unavailable");
            return;
        }
        if (existing != documents.end()) {
            const auto draft = m_environmentQueryDrafts.find(existing->identity.instance);
            if (draft != m_environmentQueryDrafts.end()) {
                if (const auto cleared = ClearEnvironmentQueryRecovery(*existing, draft->second); cleared.HasError()) {
                    existing->diagnostic = cleared.ErrorValue();
                    if (existing->identity.instance != opened.identity.instance)
                        static_cast<void>(host.CloseDocument(opened.identity.instance));
                    fail("workspace.environment_query.edit_refused");
                    return;
                }
            }
        }
        InspectEnvironmentQueryRecovery(candidate);
        if (existing != documents.end())
            m_environmentQueryDrafts.erase(existing->identity.instance);
        m_environmentQueryDrafts.merge(prepared);
        if (existing == documents.end()) {
            documents.push_back(std::move(candidate));
            return;
        }
        if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == existing->identity.instance)
            StopEnvironmentQueryPreview();
        // A source move preserves the durable AssetId. The new path owns a fresh tab key.
        if (existing->identity.instance != opened.identity.instance)
            static_cast<void>(host.CloseDocument(existing->identity.instance));
        *existing = std::move(candidate);
        static_cast<void>(host.SetDocumentDirty(existing->identity.instance, false));
        if (reloadExternal)
            m_viewModel.contentBrowserOperationError.clear();
    }

    /** @copydoc EditorWorkspaceController::CloseEnvironmentQuerySource */
    bool EditorWorkspaceController::CloseEnvironmentQuerySource(DocumentInstanceId instance) {
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto opened = std::ranges::find_if(documents, [instance](const auto &document) {
            return document.identity.instance == instance;
        });
        if (opened == documents.end())
            return false;
        if (!opened->dirty) {
            const auto draft = m_environmentQueryDrafts.find(instance);
            if (draft != m_environmentQueryDrafts.end()) {
                if (const auto cleared = ClearEnvironmentQueryRecovery(*opened, draft->second); cleared.HasError()) {
                    opened->diagnostic = cleared.ErrorValue();
                    return true;
                }
            }
        }
        if (m_viewModel.workspacePanelHost.CloseDocument(instance).HasValue()) {
            if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == instance)
                StopEnvironmentQueryPreview();
            m_environmentQueryDrafts.erase(instance);
            documents.erase(opened);
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::ResolveEnvironmentQueryClose */
    Result<bool> EditorWorkspaceController::ResolveEnvironmentQueryClose(EnvironmentQueryDocumentFence fence,
                                                                         SourceCloseDecision decision) {
        using Closed = Result<bool>;
        if (decision == SourceCloseDecision::Cancel)
            return Closed::Success(true);
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto document = std::ranges::find_if(documents, [fence](const auto &entry) {
            return entry.identity.instance == fence.instance && entry.revision == fence.revision;
        });
        const auto draft = m_environmentQueryDrafts.find(fence.instance);
        if (document == documents.end() || draft == m_environmentQueryDrafts.end())
            return Closed::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        if (decision == SourceCloseDecision::Save) {
            const auto saved = SaveEnvironmentQueryDraft(*document, draft->second);
            if (saved.HasError()) {
                document->diagnostic = saved.ErrorValue();
                return Closed::Failure(saved.ErrorValue());
            }
        } else if (decision != SourceCloseDecision::Discard) {
            return Closed::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
        }
        if (const auto cleared = ClearEnvironmentQueryRecovery(*document, draft->second); cleared.HasError())
            return Closed::Failure(cleared.ErrorValue());
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Closed::Failure(closed.ErrorValue());
        if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == fence.instance)
            StopEnvironmentQueryPreview();
        m_environmentQueryDrafts.erase(draft);
        documents.erase(document);
        return Closed::Success(true);
    }

    /** @copydoc EditorWorkspaceController::ProjectEnvironmentQueryDraft */
    void EditorWorkspaceController::ProjectEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document,
                                                                 const EnvironmentQueryDraft &draft) {
        if (document.query != draft.current.source) {
            if (m_environmentQueryPreviewWork && m_environmentQueryPreviewWork->instance == document.identity.instance)
                StopEnvironmentQueryPreview();
            document.preview = {};
        }
        document.query = draft.current.source;
        document.settings = draft.current.settings;
        document.dirty = draft.current.digest != draft.saved;
        document.canUndo = draft.history.UndoCount() != 0;
        document.canRedo = draft.history.RedoCount() != 0;
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.identity.instance, document.dirty));
    }

    /** @copydoc EditorWorkspaceController::ApplyEnvironmentQueryEdit */
    Result<void> EditorWorkspaceController::ApplyEnvironmentQueryEdit(EnvironmentQueryWorkspaceDocument &document,
                                                                      EnvironmentQueryDraft &draft,
                                                                      const EnvironmentQueryEditRequest &request) {
        using Edit = Result<void>;
        if (request.fence.revision != document.revision || document.revision == std::numeric_limits<std::uint64_t>::max())
            return Edit::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        using enum EnvironmentQueryEditAction;
        if (request.action == Save)
            return SaveEnvironmentQueryDraft(document, draft);
        if (request.action == Undo || request.action == Redo) {
            const bool redo = request.action == Redo;
            const auto *entry = draft.history.Next(redo);
            if (!entry || draft.current.source != (redo ? entry->before.source : entry->after.source))
                return Edit::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            draft.current = redo ? entry->after : entry->before;
            draft.history.DidReplay(redo);
        } else {
            if (request.action != Apply || !request.settings || !m_querySchema)
                return Edit::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
            auto prepared = PrepareQueryDraftEdit(*draft.current.source, *m_querySchema, *request.settings, request.stageChange);
            if (prepared.HasError())
                return Edit::Failure(prepared.ErrorValue());
            auto candidate = std::move(prepared).Value();
            const auto bytes = EnvironmentQuerySourceCodec::Encode(candidate.source);
            if (bytes.HasError())
                return Edit::Failure(bytes.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
            if (digest == draft.current.digest)
                return Edit::Success();
            EnvironmentQueryState next{std::make_shared<const AI::EnvironmentQueryAsset>(std::move(candidate.source)),
                                       std::move(candidate.settings), digest, bytes.Value().size()};
            auto history = draft.history.Prepare({draft.current, next, draft.current.bytes + next.bytes});
            if (!history)
                return Edit::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            draft.history.Commit(*history);
            draft.current = std::move(next);
        }
        ++document.revision;
        ProjectEnvironmentQueryDraft(document, draft);
        return Edit::Success();
    }

    /** @copydoc EditorWorkspaceController::SaveEnvironmentQueryDraft */
    Result<void> EditorWorkspaceController::SaveEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document,
                                                                      EnvironmentQueryDraft &draft) {
        JoinAiDocumentRecovery();
        auto saved = PublishEnvironmentQueryDraft(document, draft);
        if (saved.HasValue()) {
            if (auto cleared = ClearEnvironmentQueryRecovery(document, draft); cleared.HasError())
                LOG_ERROR("editor.ai_recovery", "Saved Query recovery cleanup failed: %s", cleared.ErrorValue().message.c_str());
        }
        return saved;
    }

    /** @copydoc EditorWorkspaceController::PublishEnvironmentQueryDraft */
    Result<void> EditorWorkspaceController::PublishEnvironmentQueryDraft(EnvironmentQueryWorkspaceDocument &document,
                                                                         EnvironmentQueryDraft &draft) {
        try {
            using Saved = Result<void>;
            const auto failed = [&](const Error &error) {
                document.conflict = document.conflict || error.code.Value() == Assets::AssetSourceErrors::Conflict.code.Value();
                return Saved::Failure(error);
            };
            if (!document.dirty)
                return Saved::Success();
            const auto root = ProjectRoot();
            const auto project = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "environment-query-source-save"});
            if (project.HasError())
                return failed(project.ErrorValue());
            const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
            if (!RegisteredQueryDocument(document, registry))
                return failed(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto path = Foundation::Paths::Resolve(root, registry.Find(document.asset)->sourcePath);
            const auto lock = Assets::AssetSourcePublicationLockPath(root, path);
            if (lock.HasError())
                return failed(lock.ErrorValue());
            const auto lease = m_durableFiles->TryAcquireExclusive(lock.Value(), "environment-query-source-save");
            if (lease.HasError())
                return failed(lease.ErrorValue());
            const auto captured = CaptureQuerySource(root, registry, document.asset);
            if (captured.HasError())
                return failed(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto &actual = captured.Value().Sources().front();
            const auto sidecar = CaptureQuerySidecar(root, actual.record);
            if (actual.sourceDigest != document.source || actual.bytes.size() != draft.acceptedBytes || sidecar.HasError() ||
                !sidecar.Value().exists || sidecar.Value().digest != draft.sidecar)
                return failed(MakeError(Assets::AssetSourceErrors::Conflict));

            const auto validated = AI::ReadBuiltinPointQuerySettings(*draft.current.source, *m_querySchema);
            if (validated.HasError())
                return failed(validated.ErrorValue());
            const auto encoded = EnvironmentQuerySourceCodec::Encode(*draft.current.source);
            if (encoded.HasError())
                return failed(encoded.ErrorValue());
            if (captured.Value().VerifyUnchanged().HasError())
                return failed(MakeError(Assets::AssetSourceErrors::Conflict));
            const Assets::AssetSourceIdentity accepted{true, draft.acceptedBytes, document.source};
            const auto published =
                Assets::PublishAssetSource(root, path, accepted, std::as_bytes(std::span{encoded.Value()}), lease.Value(), *m_durableFiles);
            if (published.HasError())
                return failed(published.ErrorValue());
            const auto &receipt = published.Value();
            if (receipt.status != Assets::AssetSourcePublicationStatus::Durable) {
                document.conflict = true;
                return failed(receipt.durabilityError.value_or(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)));
            }
            document.source = receipt.identity.digest;
            draft.acceptedBytes = receipt.identity.bytes;
            draft.savedSource = draft.current.source;
            draft.saved = draft.current.digest;
            document.conflict = false;
            ProjectEnvironmentQueryDraft(document, draft);
            return Saved::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        }
    }

    /** @copydoc EditorWorkspaceController::EnvironmentQueryRevisions */
    std::vector<EnvironmentQueryDocumentFence> EditorWorkspaceController::EnvironmentQueryRevisions() const {
        std::vector<EnvironmentQueryDocumentFence> revisions;
        for (const auto &document : m_viewModel.environmentQueryDocuments) {
            if (document.settings)
                revisions.push_back({document.identity.instance, document.revision});
        }
        return revisions;
    }

    /** @copydoc EditorWorkspaceController::SaveEnvironmentQueryDocuments */
    Result<void> EditorWorkspaceController::SaveEnvironmentQueryDocuments() {
        for (auto &document : m_viewModel.environmentQueryDocuments) {
            if (!document.dirty)
                continue;
            auto saved = SaveEnvironmentQueryDraft(document, m_environmentQueryDrafts.at(document.identity.instance));
            document.diagnostic = saved.HasError() ? std::optional{saved.ErrorValue()} : std::nullopt;
            if (saved.HasError())
                return saved;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ReloadExternalEnvironmentQuery */
    void EditorWorkspaceController::ReloadExternalEnvironmentQuery(EnvironmentQueryWorkspaceDocument &document,
                                                                   const EnvironmentQueryDocumentFence fence) {
        if (document.revision != fence.revision || !document.conflict)
            return;
        const auto *record = m_assetRegistry.Find(document.asset);
        if (!record)
            return;
        const auto opened = m_sourceOpenService.Open({.path = Foundation::Paths::Resolve(ProjectRoot(), record->sourcePath),
                                                      .origin = SourceOpenOrigin::Command,
                                                      .mode = SourceOpenMode::EmbeddedOnly});
        if (opened.HasError() || !opened.Value().document || opened.Value().document->identity != document.identity) {
            if (opened.HasValue() && opened.Value().document && opened.Value().document->disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(m_documentRegistry.Close(opened.Value().document->identity.instance));
            document.diagnostic = MakeError(Assets::AssetSourceErrors::Conflict);
            return;
        }
        OpenEmbeddedEnvironmentQuerySource(opened.Value(), true);
    }

    /** @copydoc EditorWorkspaceController::ProcessEnvironmentQueryEdit */
    bool EditorWorkspaceController::ProcessEnvironmentQueryEdit(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::EditEnvironmentQuery)
            return false;
        if (!command.environmentQueryEdit)
            return true;
        const auto &request = *command.environmentQueryEdit;
        if (m_viewModel.workspacePanelHost.ActiveDocument() != request.fence.instance)
            return true;
        const auto draft = m_environmentQueryDrafts.find(request.fence.instance);
        auto &documents = m_viewModel.environmentQueryDocuments;
        const auto document = std::ranges::find_if(documents, [&](const auto &entry) {
            return entry.identity.instance == request.fence.instance;
        });
        if (draft == m_environmentQueryDrafts.end() || document == documents.end())
            return true;
        try {
            if (request.action == EnvironmentQueryEditAction::ReloadExternal) {
                ReloadExternalEnvironmentQuery(*document, request.fence);
                return true;
            }
            using enum EnvironmentQueryEditAction;
            const auto edited = [&] {
                if (request.action == MergeExternal || request.action == KeepLocal)
                    return ResolveEnvironmentQueryExternal(*document, draft->second, request);
                if (request.action == RestoreRecovery || request.action == DiscardRecovery)
                    return ResolveEnvironmentQueryRecovery(*document, draft->second, request);
                return ApplyEnvironmentQueryEdit(*document, draft->second, request);
            }();
            document->diagnostic = edited.HasError() ? std::optional{edited.ErrorValue()} : std::nullopt;
        } catch (const std::bad_alloc &) {
            document->diagnostic = MakeError(Assets::AssetSourceErrors::Limit);
        }
        return true;
    }

}  // namespace Horo::Editor
