#include "AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>

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
        views.push_back({.snapshot = std::move(snapshot).Value(),
                         .usageReviewAvailable = m_mutableAssetRegistry && m_mutations && m_sourceRecoveryJobs});
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
            if (snapshot.Value().revision != found->snapshot.revision) {
                found->usageReview.reset();
                found->referencePreview.reset();
                m_blackboardReviewGenerations.erase(document.Identity().instance);
                if (m_blackboardUsageWork && m_blackboardUsageWork->fence.instance == document.Identity().instance)
                    static_cast<void>(m_blackboardUsageWork->job.RequestCancel());
            }
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
                case ReviewUsages:
                    return StartBlackboardUsageReview(request, *document);
            }
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
        };
        RefreshBlackboardDocument(*document, execute());
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
        auto validation = request.replacement ? CaptureDecisionValidation(*preview, m_gameplayRegistry.get(), m_querySchema)
                                              : Application::BlackboardRetargetValidationInput{};
        auto generation = validation.nodes;
        auto result = std::make_shared<std::shared_ptr<const Application::BlackboardUsageReview>>();
        auto job = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                      [root = m_sourceOpenService.ProjectRoot(), assets = m_mutableAssetRegistry,
                                                       mutations = m_mutations, complete, selection, providerMetadata, result,
                                                       validation = std::move(validation)](const CancellationToken &cancellation) {
            auto capture = Application::CaptureBlackboardUsageReview(root, *assets, complete, *mutations, selection, providerMetadata,
                                                                     cancellation, validation);
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
                m_blackboardReviewGenerations.erase(view.snapshot.document.instance);
            }
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
        if (decision == SourceCloseDecision::Save && current.Value().dirty) {
            const auto saved = SaveBlackboardDocument(*document, fence.revision);
            RefreshBlackboardDocument(*document, saved);
            if (saved.HasError())
                return Result<bool>::Failure(saved.ErrorValue());
        }
        const auto closed = m_viewModel.workspacePanelHost.CloseDocument(fence.instance, WorkspaceDocumentClosePolicy::DiscardChanges);
        if (closed.HasError())
            return Result<bool>::Failure(closed.ErrorValue());
        if (m_blackboardUsageWork && m_blackboardUsageWork->fence.instance == fence.instance)
            static_cast<void>(m_blackboardUsageWork->job.RequestCancel());
        m_blackboardReviewGenerations.erase(fence.instance);
        std::erase_if(m_viewModel.blackboardDocuments, [fence](const auto &entry) {
            return entry.snapshot.document.instance == fence.instance;
        });
        std::erase_if(m_blackboardDocuments, [document](const auto &owner) {
            return owner.get() == document;
        });
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Editor
