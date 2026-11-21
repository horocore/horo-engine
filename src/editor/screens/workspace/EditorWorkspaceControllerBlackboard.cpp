#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects an already registered schema identity under the Project's authored asset lease. */
        Result<void> AdmitBlackboardIdentity(const std::filesystem::path &root, AI::BlackboardSchemaId identity) {
            const auto registry = AuthoredAiSources::CurrentRegistry(root);
            if (registry.HasError())
                return Result<void>::Failure(registry.ErrorValue());
            const auto capture = AuthoredAiSources::Capture(root, registry.Value(), AI::BlackboardSchemaAssetType,
                                                            AI::BlackboardSchemaSourceCodec::MaximumBytes);
            if (capture.HasError())
                return Result<void>::Failure(capture.ErrorValue());
            for (const auto &source : capture.Value().Sources()) {
                const auto schema = AI::BlackboardSchemaSourceCodec::Decode(source.bytes);
                if (schema.HasError())
                    return Result<void>::Failure(schema.ErrorValue());
                if (schema.Value().schema.Identity() == identity)
                    return Result<void>::Failure(MakeError(AI::AIErrors::DescriptorConflict));
            }
            return capture.Value().VerifyUnchanged();
        }

        /** @brief Checks the detached metadata owner and any revocable compiler authority behind the report. */
        bool HasCurrentReviewAuthority(const Application::BlackboardUsageReview &report,
                                       const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &current) noexcept {
            if (report.providers.owner_before(current) || current.owner_before(report.providers))
                return false;
            if (!report.decisionValidation || report.decisionValidation->roots == 0)
                return true;
            const auto generation = report.decisionValidation->nodes.lock();
            return generation && generation->CanUse();
        }

        /** @brief Pins authority for explicit Decision validation without exposing it in the usage report. */
        Application::BlackboardRetargetValidationInput CaptureDecisionValidation(const BlackboardReferenceEditPreview &preview,
                                                                                 const ProjectGameplayRegistry *gameplay,
                                                                                 const AI::QuerySchemaRegistry *queries) {
            Application::BlackboardRetargetValidationInput validation{.proposedSchema = preview.schema};
            if (gameplay) {
                auto generation = gameplay->AcquireDecisionAuthoringGeneration();
                if (generation.HasValue())
                    validation.nodes = std::move(generation).Value();
            }
            if (!validation.nodes) {
                // Intrinsic graphs can compile without extensions. An empty catalog still rejects unresolved external nodes.
                auto intrinsic = AI::DecisionNodeGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
                if (intrinsic.HasValue())
                    validation.nodes = std::move(intrinsic).Value();
            }
            if (queries)
                validation.queries = std::make_shared<const AI::QuerySchemaRegistry>(*queries);
            return validation;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::PublishInitialBlackboard */
    void EditorWorkspaceController::PublishInitialBlackboard(const std::filesystem::path &destination,
                                                             const BlackboardCreationRequest &request, Assets::AssetId newId) {
        constexpr std::string_view Failed = "workspace.blackboard.create_failed";
        auto &error = m_viewModel.contentBrowserOperationError;
        error = Failed;
        if (!newId.IsValid() || !AI::IsValidBlackboardDisplayName(request.keyName) || request.key.key.IsValid() ||
            (request.key.presence == AI::BlackboardKeyPresence::Required && !request.key.defaultValue))
            return;
        try {
            const auto root = ProjectRoot();
            const auto path = ProjectPath::Parse(WorkspacePortablePathText(destination.lexically_relative(root)));
            const auto type = Assets::AssetTypeId::Parse(AI::BlackboardSchemaAssetType);
            const auto identity = AuthoredAiSources::Issue<AI::BlackboardSchemaId>();
            if (path.HasError() || type.HasError() || identity.HasError())
                return;
            auto key = request.key;
            // The first schema-local key starts the document's existing monotonic identity sequence.
            key.key = AI::BlackboardKeyId::Create(1).Value();
            const std::array keys{key};
            const auto schema = AI::BlackboardSchema::Capture({.identity = identity.Value(), .keys = keys});
            if (schema.HasError())
                return;
            const std::array rows{AI::BlackboardSchemaAuthoringRow{key.key, request.keyName}};
            const auto encoded = AI::BlackboardSchemaSourceCodec::Encode(schema.Value(), rows);
            if (encoded.HasError())
                return;
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const Application::PreparedAuthoredProjectAssetBatch batch{newId.ToString(),
                                                                       root,
                                                                       {{newId, type.Value(), path.Value(), {bytes.begin(), bytes.end()}}}};
            Application::ProjectAssetImportCommitter writer{*m_mutations, *m_durableFiles, m_mutableAssetRegistry};
            const auto written = writer.CommitAuthored(batch, {}, [&] {
                return AdmitBlackboardIdentity(root, identity.Value());
            });
            if (FinishAuthoredAiPublication(writer.LastReceipt(), written, Failed))
                OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        } catch (...) {
            // The shared authored publisher contains native failures and retains the existing receipt.
        }
    }

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
        m_blackboardRecovery.try_emplace(opened.identity.instance);
        auto &view = views.emplace_back(
            BlackboardWorkspaceDocument{.snapshot = std::move(snapshot).Value(),
                                        .usageReviewAvailable = m_mutableAssetRegistry && m_mutations && m_sourceRecoveryJobs});
        InspectBlackboardRecovery(view);
    }

    /** @copydoc EditorWorkspaceController::SaveBlackboardDocument */
    Result<void> EditorWorkspaceController::SaveBlackboardDocument(BlackboardSchemaDocument &document, const std::uint64_t revision) {
        if (!m_mutations)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        JoinAiDocumentRecovery();
        const auto current = document.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        NativeDurableFileSystem fallback;
        auto &files = m_durableFiles ? *m_durableFiles : fallback;
        auto saved = HasPendingBlackboardReferenceEdit(document.Identity().instance)
                         ? SaveBlackboardReferenceEdit(document, revision)
                         : document.Save(revision, m_assetRegistry, *m_mutations, files);
        if (saved.HasValue()) {
            if (const auto cleared = ClearBlackboardRecovery(current.Value()); cleared.HasError())
                LOG_ERROR("editor.ai_recovery", "Saved Blackboard recovery cleanup failed: %s", cleared.ErrorValue().message.c_str());
        }
        return saved;
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
            if (snapshot.Value().revision != found->snapshot.revision) {
                found->usageReview.reset();
                found->referencePreview.reset();
                found->referenceEditReady = false;
                m_blackboardReviewGenerations.erase(document.Identity().instance);
                if (m_blackboardUsageWork && m_blackboardUsageWork->fence.instance == document.Identity().instance)
                    static_cast<void>(m_blackboardUsageWork->job.RequestCancel());
            }
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(document.Identity().instance, snapshot.Value().dirty));
            found->snapshot = std::move(snapshot).Value();
            found->referenceEditStaged = HasPendingBlackboardReferenceEdit(document.Identity().instance);
        }
        found->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
    }

    /** @copydoc EditorWorkspaceController::CanApplyBlackboardReferenceEdit */
    bool EditorWorkspaceController::CanApplyBlackboardReferenceEdit(const BlackboardWorkspaceDocument &view) const {
        using Scope = Application::BlackboardPerceptionUsageScope;
        const auto &review = view.usageReview;
        if (!review || !view.referencePreview || !review->commitEvidence || !review->gaps.empty() ||
            review->perceptionScope != Scope::RegisteredProjectSources || view.snapshot.dirty || view.snapshot.conflict ||
            view.reviewingUsages || m_blackboardReferenceGroup)
            return false;
        const auto &documents = m_viewModel.decisionGraphDocuments;
        return std::ranges::all_of(review->decisionRetargets, [&](const auto &candidate) {
            const auto graph = std::ranges::find(documents, candidate.original.asset, &DecisionGraphWorkspaceDocument::asset);
            return graph != documents.end() && graph->editable && !graph->dirty && !graph->conflict &&
                   graph->source == candidate.original.source && m_decisionGraphDrafts.contains(graph->identity.instance);
        });
    }

    /** @copydoc EditorWorkspaceController::HasPendingBlackboardReferenceEdit */
    bool EditorWorkspaceController::HasPendingBlackboardReferenceEdit(DocumentInstanceId instance) const {
        return m_blackboardReferenceGroup && m_blackboardReferenceGroup->applied && InBlackboardReferenceEdit(instance);
    }

    /** @copydoc EditorWorkspaceController::InBlackboardReferenceEdit */
    bool EditorWorkspaceController::InBlackboardReferenceEdit(DocumentInstanceId instance) const {
        return m_blackboardReferenceGroup &&
               (m_blackboardReferenceGroup->schema == instance ||
                std::ranges::find(m_blackboardReferenceGroup->decisions, instance) != m_blackboardReferenceGroup->decisions.end());
    }

    /** @copydoc EditorWorkspaceController::PublishBlackboardReferenceEdit */
    bool EditorWorkspaceController::PublishBlackboardReferenceEdit(BlackboardSchemaDocument &document,
                                                                   BlackboardSchemaDocument::PreparedReferenceChange &schema,
                                                                   std::vector<PreparedDecisionReference> &decisions) {
        if (!document.CanPublishReferenceChange(schema) || !std::ranges::all_of(decisions, [](const auto &part) {
            return part.draft->history.CanPublish(part.reservation, CaptureDecisionHistoryFence(*part.document, *part.draft));
        }))
            return false;
        // Every root and history is fenced on this owner lane. No notification or callbacks run between swaps.
        static_cast<void>(document.PublishReferenceChange(schema));
        for (auto &part : decisions)
            static_cast<void>(PublishDecisionGraphChange(*part.document, *part.draft, part.reservation));
        return true;
    }

    /** @copydoc EditorWorkspaceController::ApplyBlackboardReferenceEdit */
    Result<void> EditorWorkspaceController::ApplyBlackboardReferenceEdit(const BlackboardWorkspaceRequest &request,
                                                                         BlackboardSchemaDocument &document) {
        const auto view = std::ranges::find_if(m_viewModel.blackboardDocuments, [&](const auto &entry) {
            return entry.snapshot.document.instance == request.fence.instance;
        });
        if (!m_mutations || !m_mutableAssetRegistry || !request.expectedReview || view == m_viewModel.blackboardDocuments.end() ||
            view->usageReview != request.expectedReview || view->snapshot.revision != request.fence.revision ||
            !CanApplyBlackboardReferenceEdit(*view) || m_nextBlackboardReferenceGroup == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        const auto &review = *request.expectedReview;
        const auto root = ProjectRoot();
        const auto lease = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "blackboard-reference-apply"});
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        const auto providers = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
        const auto nodes = review.decisionValidation->nodes.lock();
        if (const auto verified =
                Application::VerifyBlackboardUsageCommitFence(root, *m_mutableAssetRegistry, lease.Value(), review, providers, nodes);
            verified.HasError())
            return verified;

        const auto key = review.selection.key;
        const auto keys = view->referencePreview->schema->Keys();
        const auto descriptor = std::ranges::lower_bound(keys, key, {}, &AI::BlackboardKeyDescriptor::key);
        BlackboardSchemaEdit edit = RemoveBlackboardKey{key};
        if (descriptor != keys.end() && descriptor->key == key)
            edit = EditBlackboardKey{*descriptor};
        BlackboardReferenceGroup group{m_nextBlackboardReferenceGroup, request.fence.instance, {}, request.expectedReview, nodes};
        auto schema = document.PrepareReferenceChange(request.fence.revision, edit, group.identity);
        if (schema.HasError())
            return Result<void>::Failure(schema.ErrorValue());
        std::vector<PreparedDecisionReference> decisions;
        decisions.reserve(review.decisionRetargets.size());
        group.decisions.reserve(review.decisionRetargets.size());
        const auto registry = m_mutableAssetRegistry->Snapshot();
        for (const auto &candidate : review.decisionRetargets) {
            auto &documents = m_viewModel.decisionGraphDocuments;
            const auto graph = std::ranges::find(documents, candidate.original.asset, &DecisionGraphWorkspaceDocument::asset);
            const auto *record = registry.Find(candidate.original.asset);
            if (!record || graph == documents.end())
                return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
            const auto instance = graph->identity.instance;
            auto &draft = m_decisionGraphDrafts.at(instance);
            auto prepared =
                PrepareDecisionReferenceChange(*graph, draft, graph->revision,
                                               {*record, candidate.candidate, candidate.original.source, candidate.original.metadata},
                                               group.identity);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            group.decisions.push_back(instance);
            decisions.push_back({&*graph, &draft, std::move(prepared).Value()});
        }
        if (m_mutableAssetRegistry->Snapshot().Revision() != review.registry || !HasCurrentReviewAuthority(review, providers) ||
            !PublishBlackboardReferenceEdit(document, *schema.Value(), decisions))
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        m_blackboardReferenceGroup.emplace(std::move(group));
        ++m_nextBlackboardReferenceGroup;
        for (const auto &part : decisions)
            ProjectDecisionGraphDraft(*part.document, *part.draft);
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ReplayBlackboardReferenceEdit */
    Result<void> EditorWorkspaceController::ReplayBlackboardReferenceEdit(std::uint64_t group, bool redo) {
        if (!m_blackboardReferenceGroup || m_blackboardReferenceGroup->identity != group || m_blackboardReferenceGroup->saving ||
            m_blackboardReferenceGroup->applied == redo || !m_mutations || !m_mutableAssetRegistry)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::HistoryUnavailable));
        auto &entry = *m_blackboardReferenceGroup;
        auto *document = FindBlackboardDocument(entry.schema);
        if (!document)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        const auto root = ProjectRoot();
        const auto lease = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "blackboard-reference-replay"});
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        if (redo) {
            const auto providers = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
            if (const auto current = Application::VerifyBlackboardUsageCommitFence(root, *m_mutableAssetRegistry, lease.Value(),
                                                                                   *entry.review, providers, entry.nodes);
                current.HasError())
                return current;
        }
        const auto snapshot = document->Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        auto prepared = document->PrepareReferenceReplay(snapshot.Value().revision, group, redo);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        std::vector<PreparedDecisionReference> decisions;
        decisions.reserve(entry.decisions.size());
        for (const auto instance : entry.decisions) {
            auto &documents = m_viewModel.decisionGraphDocuments;
            const auto graph = std::ranges::find_if(documents, [instance](const auto &candidate) {
                return candidate.identity.instance == instance;
            });
            const auto draft = m_decisionGraphDrafts.find(instance);
            if (graph == documents.end() || draft == m_decisionGraphDrafts.end())
                return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
            auto next = draft->second.history.PrepareReplay(CaptureDecisionHistoryFence(*graph, draft->second), redo, group);
            if (next.HasError())
                return Result<void>::Failure(next.ErrorValue());
            decisions.push_back({&*graph, &draft->second, std::move(next).Value()});
        }
        if (!PublishBlackboardReferenceEdit(*document, *prepared.Value(), decisions))
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        entry.applied = redo;
        for (const auto &part : decisions)
            ProjectDecisionGraphDraft(*part.document, *part.draft);
        RefreshBlackboardDocument(*document, Result<void>::Success());
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::SaveBlackboardReferenceEdit */
    Result<void> EditorWorkspaceController::SaveBlackboardReferenceEdit(BlackboardSchemaDocument &document, std::uint64_t revision) {
        auto &group = *m_blackboardReferenceGroup;
        const auto snapshot = document.Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (!m_mutableAssetRegistry || !m_mutations || group.schema != document.Identity().instance ||
            snapshot.Value().revision != revision || snapshot.Value().conflict)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        const auto root = ProjectRoot();
        const auto schemaPath = root / std::filesystem::u8path(document.Identity().key.source.Value());
        const auto currentSchema = Assets::CaptureAssetSource(root, schemaPath, AI::BlackboardSchemaSourceCodec::MaximumBytes);
        if (currentSchema.HasError())
            return Result<void>::Failure(currentSchema.ErrorValue());
        if (!currentSchema.Value().identity.exists || currentSchema.Value().identity.digest != group.review->selection.source)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto &documents = m_viewModel.decisionGraphDocuments;
        // Every replacement key is already valid in the old saved schema. A successful write prefix is safe to retry.
        for (const auto instance : group.decisions) {
            const auto graph = std::ranges::find_if(documents, [instance](const auto &candidate) {
                return candidate.identity.instance == instance;
            });
            const auto draft = m_decisionGraphDrafts.find(instance);
            if (graph == documents.end() || draft == m_decisionGraphDrafts.end() || graph->conflict)
                return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
            if (graph->dirty) {
                const auto persisted = SaveDecisionGraphDraft(*graph, draft->second, true);
                if (persisted.HasError()) {
                    group.saving = group.saving || graph->conflict;
                    return persisted;
                }
                group.saving = true;
            }
        }
        auto selection = group.review->selection;
        selection.replacement.reset();
        const auto providers = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
        const auto validation = CaptureDecisionValidation({.schema = snapshot.Value().schema}, m_gameplayRegistry.get(), m_querySchema);
        std::optional<Assets::AssetRegistryRevision> complete;
        if (m_collisionInventory && m_collisionInventory->status == Assets::AssetRegistryBuildStatus::Complete)
            complete = m_collisionInventory->revision;
        const auto afterGraphs =
            Application::CaptureBlackboardUsageReview(root, *m_mutableAssetRegistry, complete, *m_mutations, selection, providers, {},
                                                      validation, Application::BlackboardPerceptionUsageScope::RegisteredProjectSources);
        if (afterGraphs.HasError())
            return Result<void>::Failure(afterGraphs.ErrorValue());
        if (!afterGraphs.Value().gaps.empty() || !afterGraphs.Value().commitEvidence)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        NativeDurableFileSystem fallback;
        auto &files = m_durableFiles ? *m_durableFiles : fallback;
        const auto saved =
            document.Save(revision, m_mutableAssetRegistry->Snapshot(), *m_mutations, files, {}, [&](const ProjectMutationLease &lease) {
            const auto current = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
            const auto nodes = afterGraphs.Value().decisionValidation->nodes.lock();
            return Application::VerifyBlackboardUsageCommitFence(root, *m_mutableAssetRegistry, lease, afterGraphs.Value(), current, nodes);
        });
        if (saved.HasValue())
            RetireBlackboardReferenceEdit();
        return saved;
    }

    /** @copydoc EditorWorkspaceController::RetireBlackboardReferenceEdit */
    void EditorWorkspaceController::RetireBlackboardReferenceEdit() {
        const auto group = std::exchange(m_blackboardReferenceGroup, std::nullopt);
        if (!group)
            return;
        if (auto *document = FindBlackboardDocument(group->schema)) {
            document->RetireReferenceGroup(group->identity);
            RefreshBlackboardDocument(*document, Result<void>::Success());
        }
        for (auto &document : m_viewModel.decisionGraphDocuments) {
            const auto instance = document.identity.instance;
            if (std::ranges::find(group->decisions, instance) == group->decisions.end())
                continue;
            if (auto draft = m_decisionGraphDrafts.find(instance); draft != m_decisionGraphDrafts.end()) {
                draft->second.history.RetireReferenceGroup(group->identity);
                ProjectDecisionGraphDraft(document, draft->second);
            }
        }
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
            const auto snapshot = document->Snapshot();
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            if (snapshot.Value().revision != request.fence.revision)
                return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
            using enum BlackboardDocumentOperation;
            switch (request.operation) {
                case Edit:
                    if (HasPendingBlackboardReferenceEdit(request.fence.instance))
                        return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
                    if (request.edit) {
                        const auto result = document->Execute(request.fence.revision, *request.edit);
                        if (result.HasValue() && m_blackboardReferenceGroup && m_blackboardReferenceGroup->schema == request.fence.instance)
                            RetireBlackboardReferenceEdit();
                        return result;
                    }
                    break;
                case Undo:
                    if (const auto group = document->NextReferenceGroup(false))
                        return ReplayBlackboardReferenceEdit(*group, false);
                    return document->Undo(request.fence.revision);
                case Redo:
                    if (const auto group = document->NextReferenceGroup(true))
                        return ReplayBlackboardReferenceEdit(*group, true);
                    return document->Redo(request.fence.revision);
                case Save:
                    return SaveBlackboardDocument(*document, request.fence.revision);
                case Reload:
                case ReloadDiscardingDraft: {
                    if (HasPendingBlackboardReferenceEdit(request.fence.instance))
                        return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
                    const bool discard = request.operation == ReloadDiscardingDraft;
                    if (discard && !snapshot.Value().conflict)
                        return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
                    JoinAiDocumentRecovery(request.fence.instance);
                    const auto result = document->Reload(request.fence.revision, m_assetRegistry, discard);
                    if (result.HasValue()) {
                        if (m_blackboardReferenceGroup && m_blackboardReferenceGroup->schema == request.fence.instance)
                            RetireBlackboardReferenceEdit();
                        if (const auto cleared = ClearBlackboardRecovery(snapshot.Value()); cleared.HasError())
                            LOG_ERROR("editor.ai_recovery", "Reloaded Blackboard recovery cleanup failed: %s",
                                      cleared.ErrorValue().message.c_str());
                    }
                    return result;
                }
                case RestoreRecovery:
                case DiscardRecovery:
                    return ResolveBlackboardRecovery(*document, request);
                case MergeExternal:
                case KeepLocal: {
                    if (HasPendingBlackboardReferenceEdit(request.fence.instance))
                        return Result<void>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
                    JoinAiDocumentRecovery(request.fence.instance);
                    auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
                    const auto result = document->ResolveExternal(request.fence.revision, registry, request.operation == KeepLocal);
                    if (result.HasValue() && m_blackboardReferenceGroup && m_blackboardReferenceGroup->schema == request.fence.instance)
                        RetireBlackboardReferenceEdit();
                    return result;
                }
                case ReviewUsages:
                    return StartBlackboardUsageReview(request, *document);
                case ApplyReviewedEdit:
                    return ApplyBlackboardReferenceEdit(request, *document);
            }
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        };
        try {
            RefreshBlackboardDocument(*document, execute());
        } catch (const std::bad_alloc &) {
            RefreshBlackboardDocument(*document, Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit)));
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::StartBlackboardUsageReview */
    Result<void> EditorWorkspaceController::StartBlackboardUsageReview(const BlackboardWorkspaceRequest &request,
                                                                       const BlackboardSchemaDocument &document) {
        if (!m_mutableAssetRegistry || !m_mutations || !m_sourceRecoveryJobs)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        if (request.reviewKey.has_value() == request.edit.has_value())
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        if (request.replacement && (!request.edit || !std::holds_alternative<RemoveBlackboardKey>(*request.edit)))
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        if (m_blackboardUsageWork)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::Busy));
        const auto current = document.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &snapshot = current.Value();
        if (snapshot.revision != request.fence.revision)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        if (snapshot.dirty || snapshot.conflict || !snapshot.sourceDigest)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::Dirty));
        auto key = request.reviewKey;
        std::optional<BlackboardReferenceEditPreview> preview;
        if (request.edit) {
            auto candidate = document.PreviewReferenceEdit(request.fence.revision, *request.edit);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            preview = std::move(candidate).Value();
            key = preview->key;
        }
        if (!std::ranges::binary_search(snapshot.durableSchema->Keys(), *key, {}, &AI::BlackboardKeyDescriptor::key))
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));

        const Application::BlackboardUsageSelection selection{snapshot.source->asset,
                                                              snapshot.durableSchema->Identity(),
                                                              snapshot.durableSchema->Version(),
                                                              *key,
                                                              *snapshot.sourceDigest,
                                                              request.replacement};
        std::optional<Assets::AssetRegistryRevision> complete;
        if (m_collisionInventory && m_collisionInventory->status == Assets::AssetRegistryBuildStatus::Complete)
            complete = m_collisionInventory->revision;
        const auto providerMetadata = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
        auto validation = preview ? CaptureDecisionValidation(*preview, m_gameplayRegistry.get(), m_querySchema)
                                  : Application::BlackboardRetargetValidationInput{};
        auto generation = validation.nodes;
        auto result = std::make_shared<std::shared_ptr<const Application::BlackboardUsageReview>>();
        auto job = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                      [root = m_sourceOpenService.ProjectRoot(), assets = m_mutableAssetRegistry,
                                                       mutations = m_mutations, complete, selection, providerMetadata, result,
                                                       validation = std::move(validation)](const CancellationToken &cancellation) {
            auto capture =
                Application::CaptureBlackboardUsageReview(root, *assets, complete, *mutations, selection, providerMetadata, cancellation,
                                                          validation,
                                                          validation.proposedSchema
                                                              ? Application::BlackboardPerceptionUsageScope::RegisteredProjectSources
                                                              : Application::BlackboardPerceptionUsageScope::IncludeExternalBindings);
            if (capture.HasError())
                return Result<void>::Failure(capture.ErrorValue());
            *result = std::make_shared<const Application::BlackboardUsageReview>(std::move(capture).Value());
            return Result<void>::Success();
        });
        if (job.HasError())
            return Result<void>::Failure(job.ErrorValue());
        m_blackboardUsageWork.emplace(
            BlackboardUsageWork{std::move(job).Value(), request.fence, std::move(result), std::move(preview), std::move(generation)});
        m_blackboardReviewGenerations.erase(request.fence.instance);
        const auto view = std::ranges::find_if(m_viewModel.blackboardDocuments, [&](const auto &entry) {
            return entry.snapshot.document.instance == request.fence.instance;
        });
        if (view != m_viewModel.blackboardDocuments.end()) {
            view->reviewingUsages = true;
            view->usageReview.reset();
            view->referencePreview.reset();
            view->referenceEditReady = false;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::UpdateBlackboardUsageReview */
    void EditorWorkspaceController::UpdateBlackboardUsageReview() {
        const auto providerMetadata = m_gameplayRegistry ? m_gameplayRegistry->DecisionUsageMetadata() : nullptr;
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot().Revision() : Assets::AssetRegistryRevision{};
        for (auto &view : m_viewModel.blackboardDocuments) {
            if (view.usageReview &&
                (view.usageReview->registry != registry || !HasCurrentReviewAuthority(*view.usageReview, providerMetadata))) {
                view.usageReview.reset();
                view.referencePreview.reset();
                view.referenceEditReady = false;
                m_blackboardReviewGenerations.erase(view.snapshot.document.instance);
            }
            view.referenceEditReady = CanApplyBlackboardReferenceEdit(view);
        }
        if (!m_blackboardUsageWork)
            return;
        const auto job = m_blackboardUsageWork->job.Snapshot();
        if (!job || !job->terminalResult)
            return;
        auto work = std::move(*m_blackboardUsageWork);
        m_blackboardUsageWork.reset();
        const auto view = std::ranges::find_if(m_viewModel.blackboardDocuments, [&](const auto &entry) {
            return entry.snapshot.document.instance == work.fence.instance;
        });
        if (view == m_viewModel.blackboardDocuments.end())
            return;
        view->reviewingUsages = false;
        if (view->snapshot.revision != work.fence.revision || job->state == JobState::Cancelled)
            return;
        if (job->state != JobState::Succeeded) {
            view->diagnostic = job->terminalResult->error;
            return;
        }
        auto result = std::move(*work.result);
        if (result && result->registry == registry && HasCurrentReviewAuthority(*result, providerMetadata)) {
            if (result->decisionValidation && result->decisionValidation->roots != 0)
                m_blackboardReviewGenerations.emplace(work.fence.instance, std::move(work.generation));
            view->usageReview = std::move(result);
            view->referencePreview = std::move(work.preview);
            view->referenceEditReady = CanApplyBlackboardReferenceEdit(*view);
        } else {
            view->diagnostic = MakeError(Application::DecisionCookErrors::Stale);
        }
    }

    /** @copydoc EditorWorkspaceController::StopBlackboardUsageReview */
    void EditorWorkspaceController::StopBlackboardUsageReview() {
        m_blackboardReviewGenerations.clear();
        if (!m_blackboardUsageWork)
            return;
        static_cast<void>(m_blackboardUsageWork->job.RequestCancel());
        static_cast<void>(m_blackboardUsageWork->job.Wait());
        m_blackboardUsageWork.reset();
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
        if (decision == SourceCloseDecision::Discard && HasPendingBlackboardReferenceEdit(fence.instance))
            return Result<bool>::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        if (decision == SourceCloseDecision::Save && current.Value().dirty) {
            const auto saved = SaveBlackboardDocument(*document, fence.revision);
            RefreshBlackboardDocument(*document, saved);
            if (saved.HasError())
                return Result<bool>::Failure(saved.ErrorValue());
        }
        if (const auto cleared = ClearBlackboardRecovery(current.Value()); cleared.HasError())
            return Result<bool>::Failure(cleared.ErrorValue());
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Result<bool>::Failure(closed.ErrorValue());
        if (m_blackboardReferenceGroup && m_blackboardReferenceGroup->schema == fence.instance)
            RetireBlackboardReferenceEdit();
        if (m_blackboardUsageWork && m_blackboardUsageWork->fence.instance == fence.instance)
            static_cast<void>(m_blackboardUsageWork->job.RequestCancel());
        m_blackboardReviewGenerations.erase(fence.instance);
        m_blackboardRecovery.erase(fence.instance);
        std::erase_if(m_viewModel.blackboardDocuments, [fence](const auto &entry) {
            return entry.snapshot.document.instance == fence.instance;
        });
        std::erase_if(m_blackboardDocuments, [document](const auto &owner) {
            return owner.get() == document;
        });
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Editor
