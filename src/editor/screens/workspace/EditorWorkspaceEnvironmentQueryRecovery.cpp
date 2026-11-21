#include "EnvironmentQueryWorkspaceSource.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <chrono>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Immutable Query source and accepted dependencies captured before scheduling I/O. */
        struct QueryRecoveryCapture final {
            Assets::AssetId asset;
            DocumentIdentity document;
            Assets::AssetId session;
            std::uint64_t revision;
            Assets::AssetSourceIdentity acceptedSource;
            Sha256Digest sidecar;
            std::shared_ptr<const AI::EnvironmentQueryAsset> query;
            std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery;
        };

        /** @brief Identifies versioned Query recovery bytes followed by the accepted metadata digest. */
        constexpr std::size_t QueryRecoveryHeaderSize = 1 + Sha256Digest{}.bytes.size();

        /** @brief Encodes the Query draft using its registered source codec. */
        Result<std::vector<std::byte>> EncodeQueryRecovery(const QueryRecoveryCapture &capture) {
            using Encoded = Result<std::vector<std::byte>>;
            const auto encoded = AI::EnvironmentQuerySourceCodec::Encode(*capture.query);
            if (encoded.HasError())
                return Encoded::Failure(encoded.ErrorValue());
            const auto source = std::as_bytes(std::span{encoded.Value()});
            if (source.size() > DocumentRecoveryLimits{}.maximumRecordBytes - QueryRecoveryHeaderSize)
                return Encoded::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            const auto sidecar = std::as_bytes(std::span{capture.sidecar.bytes});
            std::vector<std::byte> payload;
            payload.reserve(QueryRecoveryHeaderSize + source.size());
            payload.push_back(std::byte{1});
            payload.insert(payload.end(), sidecar.begin(), sidecar.end());
            payload.insert(payload.end(), source.begin(), source.end());
            return Encoded::Success(std::move(payload));
        }

        /** @brief Writes an immutable Query checkpoint or transfers the exact accepted candidate. */
        Result<void> WriteQueryRecovery(const QueryRecoveryCapture &capture, DocumentRecoveryService &recovery,
                                        const CancellationToken &cancellation = {}) {
            try {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                auto payload = EncodeQueryRecovery(capture);
                if (payload.HasError())
                    return Result<void>::Failure(std::move(payload).ErrorValue());
                const auto now =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const DocumentRecoveryRecord record{capture.asset,
                                                    capture.document.key,
                                                    capture.session,
                                                    capture.document.instance.Value(),
                                                    capture.revision,
                                                    static_cast<std::uint64_t>(now),
                                                    capture.acceptedSource,
                                                    ComputeSha256(payload.Value()),
                                                    std::move(payload).Value()};
                return capture.acceptedRecovery ? recovery.ReplaceAccepted(record, *capture.acceptedRecovery, cancellation)
                                                : recovery.Write(record, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            }
        }

        /** @brief Retains only immutable Query state across the job boundary. */
        QueryRecoveryCapture CaptureQueryRecovery(const EnvironmentQueryWorkspaceDocument &document, Assets::AssetId session,
                                                  std::size_t acceptedBytes, Sha256Digest sidecar,
                                                  const std::shared_ptr<const DocumentRecoveryRecord> &accepted) {
            return {document.asset, document.identity, session, document.revision, {true, acceptedBytes, document.source},
                    sidecar,        document.query,    accepted};
        }

        /** @brief Checks ownership independent of the particular Query revision. */
        bool OwnsQueryRecovery(const DocumentRecoveryRecord &record, Assets::AssetId session, DocumentInstanceId instance) {
            return record.sessionOwner == session && record.sessionGeneration == instance.Value();
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::InspectEnvironmentQueryRecovery */
    void EditorWorkspaceController::InspectEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document) const {
        if (!document.settings || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid())
            return;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        auto observed = recovery.Inspect(document.asset, document.identity.key);
        if (observed.HasError())
            LOG_ERROR("editor.ai_recovery", "Query recovery inspection failed: %s", observed.ErrorValue().message.c_str());
        else if (observed.Value() && !OwnsQueryRecovery(*observed.Value(), m_workspaceSession, document.identity.instance))
            document.recovery = std::make_shared<const DocumentRecoveryRecord>(std::move(observed).Value().value());
    }

    /** @copydoc EditorWorkspaceController::RestoreEnvironmentQueryRecovery */
    Result<void> EditorWorkspaceController::RestoreEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document,
                                                                            EnvironmentQueryDraft &draft) {
        using namespace EnvironmentQueryWorkspaceSource;
        const auto conflict = [] {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        };
        if (document.dirty || document.conflict)
            return conflict();
        const auto &checkpoint = *document.recovery;
        const Assets::AssetSourceIdentity accepted{true, draft.acceptedBytes, document.source};
        const auto root = ProjectRoot();
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        const auto current = CaptureQuerySource(root, registry, document.asset);
        if (current.HasError() || checkpoint.acceptedSource != accepted || !RegisteredQueryDocument(document, registry))
            return conflict();
        const auto &source = current.Value().Sources().front();
        const auto sidecar = CaptureQuerySidecar(root, source.record);
        if (source.sourceDigest != accepted.digest || source.bytes.size() != accepted.bytes || sidecar.HasError() ||
            !sidecar.Value().exists || sidecar.Value().digest != draft.sidecar)
            return conflict();

        const std::span payload{checkpoint.payload};
        if (payload.size() <= QueryRecoveryHeaderSize || payload.front() != std::byte{1} || ComputeSha256(payload) != checkpoint.state)
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
        if (!std::ranges::equal(payload.subspan(1, QueryRecoveryHeaderSize - 1), std::as_bytes(std::span{draft.sidecar.bytes})))
            return conflict();
        const auto bytes = payload.subspan(QueryRecoveryHeaderSize);
        auto decoded = AI::EnvironmentQuerySourceCodec::Decode({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        if (decoded.Value().Id() != document.query->Id() || decoded.Value().ResultSchema().id != document.query->ResultSchema().id)
            return conflict();
        auto settings = AI::ReadBuiltinPointQuerySettings(decoded.Value(), *m_querySchema);
        if (settings.HasError())
            return Result<void>::Failure(settings.ErrorValue());
        const auto digest = ComputeSha256(bytes);
        if (digest == draft.saved || current.Value().VerifyUnchanged().HasError())
            return conflict();

        auto query = std::make_shared<const AI::EnvironmentQueryAsset>(std::move(decoded).Value());
        draft.current = {std::move(query), std::move(settings).Value(), digest, bytes.size()};
        draft.history.Clear();
        draft.acceptedRecovery = std::move(document.recovery);
        ++document.revision;
        ProjectEnvironmentQueryDraft(document, draft);
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ResolveEnvironmentQueryRecovery */
    Result<void> EditorWorkspaceController::ResolveEnvironmentQueryRecovery(EnvironmentQueryWorkspaceDocument &document,
                                                                            EnvironmentQueryDraft &draft,
                                                                            const EnvironmentQueryEditRequest &request) {
        if (request.fence.revision != document.revision || document.revision == std::numeric_limits<std::uint64_t>::max() ||
            !document.recovery || request.recovery != document.recovery)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        JoinAiDocumentRecovery(document.identity.instance);
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        if (request.action == EnvironmentQueryEditAction::RestoreRecovery) {
            const auto observed = recovery.Inspect(document.asset, document.identity.key);
            if (observed.HasError())
                return Result<void>::Failure(observed.ErrorValue());
            if (!observed.Value() || *observed.Value() != *document.recovery)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return RestoreEnvironmentQueryRecovery(document, draft);
        }
        const auto discarded = recovery.Discard(*document.recovery);
        if (discarded.HasValue()) {
            document.recovery.reset();
            ++document.revision;
        }
        return discarded;
    }

    /** @copydoc EditorWorkspaceController::ClearEnvironmentQueryRecovery */
    Result<void> EditorWorkspaceController::ClearEnvironmentQueryRecovery(const EnvironmentQueryWorkspaceDocument &document,
                                                                          EnvironmentQueryDraft &draft) {
        JoinAiDocumentRecovery(document.identity.instance);
        if (!draft.recoveryAttempted && !draft.acceptedRecovery)
            return Result<void>::Success();
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        const auto observed = recovery.Inspect(document.asset, document.identity.key);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        const auto &record = observed.Value();
        if (record && (OwnsQueryRecovery(*record, m_workspaceSession, document.identity.instance) ||
                       (draft.acceptedRecovery && *record == *draft.acceptedRecovery))) {
            if (const auto discarded = recovery.Discard(*record); discarded.HasError())
                return discarded;
        }
        draft.recoveryAttempted = false;
        draft.recoveryRevision = 0;
        draft.acceptedRecovery.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StartQueuedEnvironmentQueryRecovery */
    void EditorWorkspaceController::StartQueuedEnvironmentQueryRecovery(const DocumentInstanceId instance) {
        const auto &documents = m_viewModel.environmentQueryDocuments;
        const auto document = std::ranges::find_if(documents, [instance](const auto &entry) {
            return entry.identity.instance == instance;
        });
        const auto draft = m_environmentQueryDrafts.find(instance);
        if (document == documents.end() || draft == m_environmentQueryDrafts.end() || !document->dirty || document->recovery ||
            draft->second.recoveryRevision >= document->revision)
            return;
        const auto capture = CaptureQueryRecovery(*document, m_workspaceSession, draft->second.acceptedBytes, draft->second.sidecar,
                                                  draft->second.acceptedRecovery);
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                            [root = ProjectRoot(), mutations = m_mutations, files = m_durableFiles,
                                                             capture](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files);
            return WriteQueryRecovery(capture, recovery, cancellation);
        });
        if (submitted.HasError()) {
            LOG_ERROR("editor.ai_recovery", "Query recovery scheduling failed: %s", submitted.ErrorValue().message.c_str());
            return;
        }
        draft->second.recoveryAttempted = true;
        m_aiDocumentRecovery.emplace(
            AiDocumentRecoveryWork{instance, capture.revision, std::move(submitted).Value(), capture.acceptedRecovery});
    }

    /** @copydoc EditorWorkspaceController::FlushEnvironmentQueryRecovery */
    void EditorWorkspaceController::FlushEnvironmentQueryRecovery() {
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        for (const auto &document : m_viewModel.environmentQueryDocuments) {
            const auto found = m_environmentQueryDrafts.find(document.identity.instance);
            if (found == m_environmentQueryDrafts.end() || document.recovery)
                continue;
            auto &draft = found->second;
            if (!document.dirty) {
                if (const auto cleared = ClearEnvironmentQueryRecovery(document, draft); cleared.HasError())
                    LOG_ERROR("editor.ai_recovery", "Query recovery cleanup failed: %s", cleared.ErrorValue().message.c_str());
                continue;
            }
            if (draft.recoveryRevision >= document.revision)
                continue;
            const auto capture =
                CaptureQueryRecovery(document, m_workspaceSession, draft.acceptedBytes, draft.sidecar, draft.acceptedRecovery);
            draft.recoveryAttempted = true;
            if (const auto written = WriteQueryRecovery(capture, recovery); written.HasError()) {
                LOG_ERROR("editor.ai_recovery", "Query recovery checkpoint failed: %s", written.ErrorValue().message.c_str());
                continue;
            }
            draft.recoveryRevision = document.revision;
            draft.acceptedRecovery.reset();
        }
    }
}  // namespace Horo::Editor
