#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        constexpr auto PayloadVersion = std::byte{1};
        constexpr std::size_t PayloadPrefixBytes = 1 + sizeof(std::uint64_t);

        /** @brief Serializes the authored rows and key allocator boundary without borrowing the live owner. */
        Result<std::vector<std::byte>> EncodeBlackboardCheckpoint(const BlackboardSchemaDocumentSnapshot &snapshot) {
            using Encoded = Result<std::vector<std::byte>>;
            std::vector<AI::BlackboardSchemaAuthoringRow> rows;
            rows.reserve(snapshot.source->rows.size());
            for (const auto &row : snapshot.source->rows)
                rows.push_back({row.descriptor.key, row.displayName});
            const auto encoded = AI::BlackboardSchemaSourceCodec::Encode(*snapshot.schema, rows);
            if (encoded.HasError())
                return Encoded::Failure(encoded.ErrorValue());
            const auto bytes = std::as_bytes(std::span{encoded.Value()});
            const auto issued = snapshot.nextKey ? snapshot.nextKey->Value() - 1 : std::numeric_limits<std::uint64_t>::max();
            std::vector<std::byte> payload;
            payload.reserve(PayloadPrefixBytes + bytes.size());
            payload.push_back(PayloadVersion);
            for (std::size_t offset = sizeof(issued); offset > 0; --offset)
                payload.push_back(static_cast<std::byte>((issued >> ((offset - 1) * 8)) & 0xff));
            payload.insert(payload.end(), bytes.begin(), bytes.end());
            return Encoded::Success(std::move(payload));
        }

        /** @brief Writes or explicitly transfers a Blackboard checkpoint in the project-local recovery store. */
        Result<void> WriteBlackboardCheckpoint(const BlackboardSchemaDocumentSnapshot &snapshot, const Assets::AssetId session,
                                               const std::shared_ptr<const DocumentRecoveryRecord> &accepted,
                                               DocumentRecoveryService &recovery, const CancellationToken &cancellation) {
            try {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                auto payload = EncodeBlackboardCheckpoint(snapshot);
                if (payload.HasError())
                    return Result<void>::Failure(std::move(payload).ErrorValue());
                const auto now =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const DocumentRecoveryRecord record{snapshot.source->asset,
                                                    snapshot.document.key,
                                                    session,
                                                    snapshot.document.instance.Value(),
                                                    snapshot.revision,
                                                    static_cast<std::uint64_t>(now),
                                                    {true, snapshot.sourceBytes, *snapshot.sourceDigest},
                                                    ComputeSha256(payload.Value()),
                                                    std::move(payload).Value()};
                return accepted ? recovery.ReplaceAccepted(record, *accepted, cancellation) : recovery.Write(record, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            }
        }

        /** @brief Applies a validated bounded checkpoint while preserving the owner's durable schema. */
        Result<void> RestoreBlackboardCheckpoint(BlackboardSchemaDocument &document, std::uint64_t revision,
                                                 const Assets::AssetRegistrySnapshot &registry, const DocumentRecoveryRecord &record) {
            const std::span payload{record.payload};
            if (payload.size() <= PayloadPrefixBytes || payload.front() != PayloadVersion || ComputeSha256(payload) != record.state)
                return Result<void>::Failure(MakeError(BlackboardDocumentErrors::InvalidEdit));
            std::uint64_t issued{};
            for (const auto byte : payload.subspan(1, sizeof(issued)))
                issued = (issued << 8) | std::to_integer<std::uint8_t>(byte);
            const auto bytes = payload.subspan(PayloadPrefixBytes);
            return document.RestoreDraft(revision, registry, record.acceptedSource,
                                         {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, issued);
        }

        /** @brief Identifies recovery from this exact workspace and tab. */
        bool OwnsBlackboardCheckpoint(const DocumentRecoveryRecord &record, Assets::AssetId session, DocumentInstanceId instance) {
            return record.sessionOwner == session && record.sessionGeneration == instance.Value();
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::InspectBlackboardRecovery */
    void EditorWorkspaceController::InspectBlackboardRecovery(BlackboardWorkspaceDocument &view) const {
        if (!m_mutations || !m_durableFiles || !m_workspaceSession.IsValid() || !view.snapshot.sourceDigest)
            return;
        const auto &snapshot = view.snapshot;
        const DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        auto observed = recovery.Inspect(snapshot.source->asset, snapshot.document.key);
        if (observed.HasError())
            LOG_ERROR("editor.ai_recovery", "Blackboard recovery inspection failed: %s", observed.ErrorValue().message.c_str());
        else if (observed.Value() && !OwnsBlackboardCheckpoint(*observed.Value(), m_workspaceSession, snapshot.document.instance))
            view.recovery = std::make_shared<const DocumentRecoveryRecord>(std::move(observed).Value().value());
    }

    /** @copydoc EditorWorkspaceController::ResolveBlackboardRecovery */
    Result<void> EditorWorkspaceController::ResolveBlackboardRecovery(BlackboardSchemaDocument &document,
                                                                      const BlackboardWorkspaceRequest &request) {
        const auto tab = std::ranges::find_if(m_viewModel.blackboardDocuments, [&](const auto &view) {
            return view.snapshot.document == document.Identity();
        });
        if (tab == m_viewModel.blackboardDocuments.end() || !request.recovery || tab->recovery != request.recovery ||
            HasPendingBlackboardReferenceEdit(document.Identity().instance) || !m_mutations || !m_durableFiles)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        JoinAiDocumentRecovery(document.Identity().instance);
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        const auto &record = *request.recovery;
        if (request.operation == BlackboardDocumentOperation::DiscardRecovery) {
            const auto discarded = recovery.Discard(record);
            if (discarded.HasValue())
                tab->recovery.reset();
            return discarded;
        }
        const auto current = recovery.Inspect(record.asset, record.document);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (!current.Value() || *current.Value() != record)
            return Result<void>::Failure(MakeError(BlackboardDocumentErrors::StaleRevision));
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        const auto restored = RestoreBlackboardCheckpoint(document, request.fence.revision, registry, record);
        if (restored.HasValue())
            m_blackboardRecovery.at(document.Identity().instance).acceptedRecovery = std::move(tab->recovery);
        return restored;
    }

    /** @copydoc EditorWorkspaceController::ClearBlackboardRecovery */
    Result<void> EditorWorkspaceController::ClearBlackboardRecovery(const BlackboardSchemaDocumentSnapshot &snapshot) {
        const auto instance = snapshot.document.instance;
        JoinAiDocumentRecovery(instance);
        const auto found = m_blackboardRecovery.find(instance);
        if (found == m_blackboardRecovery.end() || (!found->second.recoveryAttempted && !found->second.acceptedRecovery))
            return Result<void>::Success();
        auto &draft = found->second;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        const auto current = recovery.Inspect(snapshot.source->asset, snapshot.document.key);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto &record = current.Value();
        if (record && (OwnsBlackboardCheckpoint(*record, m_workspaceSession, instance) ||
                       (draft.acceptedRecovery && *record == *draft.acceptedRecovery))) {
            const auto discarded = recovery.Discard(*record);
            if (discarded.HasError())
                return discarded;
        }
        draft = {};
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::CheckpointBlackboard */
    void EditorWorkspaceController::CheckpointBlackboard(DocumentInstanceId instance, bool synchronous) {
        const auto tab = std::ranges::find_if(m_viewModel.blackboardDocuments, [instance](const auto &view) {
            return view.snapshot.document.instance == instance;
        });
        const auto found = m_blackboardRecovery.find(instance);
        if (tab == m_viewModel.blackboardDocuments.end() || found == m_blackboardRecovery.end() || !tab->snapshot.dirty ||
            !tab->snapshot.sourceDigest || tab->recovery || found->second.recoveryRevision >= tab->snapshot.revision ||
            HasPendingBlackboardReferenceEdit(instance))
            return;

        auto &draft = found->second;
        const auto write = [root = ProjectRoot(), mutations = m_mutations, files = m_durableFiles, session = m_workspaceSession,
                            snapshot = tab->snapshot, accepted = draft.acceptedRecovery](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files);
            return WriteBlackboardCheckpoint(snapshot, session, accepted, recovery, cancellation);
        };
        if (synchronous) {
            draft.recoveryAttempted = true;
            if (const auto saved = write({}); saved.HasError())
                LOG_ERROR("editor.ai_recovery", "Blackboard recovery checkpoint failed: %s", saved.ErrorValue().message.c_str());
            else {
                draft.recoveryRevision = tab->snapshot.revision;
                draft.acceptedRecovery.reset();
            }
            return;
        }
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io}, write);
        if (submitted.HasError()) {
            LOG_ERROR("editor.ai_recovery", "Blackboard recovery scheduling failed: %s", submitted.ErrorValue().message.c_str());
            return;
        }
        draft.recoveryAttempted = true;
        m_aiDocumentRecovery.emplace(
            AiDocumentRecoveryWork{instance, tab->snapshot.revision, std::move(submitted).Value(), draft.acceptedRecovery});
    }

    /** @copydoc EditorWorkspaceController::FlushBlackboardRecovery */
    void EditorWorkspaceController::FlushBlackboardRecovery() {
        for (const auto &tab : m_viewModel.blackboardDocuments) {
            if (tab.snapshot.dirty)
                CheckpointBlackboard(tab.snapshot.document.instance, true);
            else if (const auto cleared = ClearBlackboardRecovery(tab.snapshot); cleared.HasError())
                LOG_ERROR("editor.ai_recovery", "Blackboard recovery cleanup failed: %s", cleared.ErrorValue().message.c_str());
        }
    }
}  // namespace Horo::Editor
