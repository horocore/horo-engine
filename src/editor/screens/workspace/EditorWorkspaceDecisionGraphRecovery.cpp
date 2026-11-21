#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t LayoutIdentityOffset = 1;
        constexpr std::size_t GraphSizeOffset = LayoutIdentityOffset + 1 + 8 + Sha256Digest{}.bytes.size();
        constexpr std::size_t RecoveryHeaderSize = GraphSizeOffset + 4;

        /** @brief Immutable owner capture, with both accepted disk identities separate from in-memory changes. */
        struct DecisionRecoveryCapture final {
            Assets::AssetId asset;
            DocumentIdentity document;
            Assets::AssetId session;
            std::uint64_t revision;
            Assets::AssetSourceIdentity acceptedSource;
            Assets::AssetSourceIdentity acceptedLayout;
            std::shared_ptr<const AI::DecisionGraphSource> graph;
            std::shared_ptr<const DecisionGraphLayout> layout;
            std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery;
        };

        /** @brief Appends one fixed-width big-endian integer to the versioned record payload. */
        void AppendInteger(std::vector<std::byte> &bytes, std::uint64_t value, std::size_t width) {
            for (; width > 0; --width)
                bytes.push_back(static_cast<std::byte>((value >> ((width - 1) * 8)) & 0xff));
        }

        /** @brief Encodes graph and layout with their existing canonical codecs without writing either destination. */
        Result<std::vector<std::byte>> EncodeDecisionRecovery(const DecisionRecoveryCapture &capture) {
            using Encoded = Result<std::vector<std::byte>>;
            const auto graph = AI::DecisionPlanCodec::EncodeSource(std::span{capture.graph.get(), 1}, capture.graph->asset);
            if (graph.HasError())
                return Encoded::Failure(graph.ErrorValue());
            const auto layout = EncodeDecisionLayout(*capture.layout, capture.asset, capture.graph->asset);
            if (layout.HasError())
                return Encoded::Failure(layout.ErrorValue());
            if (graph.Value().size() + layout.Value().size() > DocumentRecoveryLimits{}.maximumRecordBytes - RecoveryHeaderSize)
                return Encoded::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            const auto source = std::as_bytes(std::span{graph.Value()});
            const auto base = std::as_bytes(std::span{capture.acceptedLayout.digest.bytes});
            std::vector<std::byte> payload;
            payload.reserve(RecoveryHeaderSize + source.size() + layout.Value().size());
            AppendInteger(payload, 1, 1);
            AppendInteger(payload, capture.acceptedLayout.exists, 1);
            AppendInteger(payload, capture.acceptedLayout.bytes, 8);
            payload.insert(payload.end(), base.begin(), base.end());
            AppendInteger(payload, source.size(), 4);
            payload.insert(payload.end(), source.begin(), source.end());
            payload.insert(payload.end(), layout.Value().begin(), layout.Value().end());
            return Encoded::Success(std::move(payload));
        }

        /** @brief Reads a validated fixed-width big-endian payload field. */
        std::uint64_t ReadInteger(const std::span<const std::byte> bytes) {
            std::uint64_t value{};
            for (const auto byte : bytes)
                value = (value << 8) | std::to_integer<std::uint8_t>(byte);
            return value;
        }

        /** @brief Decodes the exact accepted layout identity stored alongside the draft. */
        Assets::AssetSourceIdentity ReadLayoutIdentity(const std::span<const std::byte> payload) {
            Sha256Digest digest;
            const auto bytes = payload.subspan(LayoutIdentityOffset + 9, digest.bytes.size());
            std::ranges::transform(bytes, digest.bytes.begin(), [](const std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return {payload[LayoutIdentityOffset] == std::byte{1},
                    static_cast<std::size_t>(ReadInteger(payload.subspan(LayoutIdentityOffset + 1, 8))), digest};
        }

        /** @brief Validates the checkpoint framing and reuses both registered source codecs. */
        Result<DecisionGraphDocumentRoot> DecodeDecisionRecovery(const DocumentRecoveryRecord &record,
                                                                 const DecisionGraphWorkspaceDocument &document,
                                                                 const Assets::AssetSourceIdentity &acceptedLayout) {
            using Root = Result<DecisionGraphDocumentRoot>;
            const std::span payload{record.payload};
            if (payload.size() <= RecoveryHeaderSize || payload.front() != std::byte{1} || payload[LayoutIdentityOffset] > std::byte{1} ||
                ReadInteger(payload.subspan(LayoutIdentityOffset + 1, 8)) > MaximumDecisionLayoutBytes ||
                ComputeSha256(payload) != record.state)
                return Root::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            if (ReadLayoutIdentity(payload) != acceptedLayout)
                return Root::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto graphSize = ReadInteger(payload.subspan(GraphSizeOffset, 4));
            if (graphSize == 0 || graphSize >= payload.size() - RecoveryHeaderSize || graphSize > AI::DecisionCodecLimits{}.maximumBytes)
                return Root::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto graphBytes = payload.subspan(RecoveryHeaderSize, static_cast<std::size_t>(graphSize));
            AI::DecisionGraphAssetId graphRoot;
            auto graph = AI::DecisionPlanCodec::DecodeSource({reinterpret_cast<const std::uint8_t *>(graphBytes.data()), graphBytes.size()},
                                                             graphRoot);
            if (graph.HasError())
                return Root::Failure(graph.ErrorValue());
            if (graphRoot != document.graph->asset || graph.Value().size() != 1 ||
                graph.Value().front().body.index() != document.graph->body.index())
                return Root::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            auto layout = DecodeDecisionLayout(payload.subspan(RecoveryHeaderSize + graphBytes.size()), document.asset, graphRoot);
            if (layout.HasError())
                return Root::Failure(layout.ErrorValue());
            auto graphs = std::move(graph).Value();
            return Root::Success({std::make_shared<const AI::DecisionGraphSource>(std::move(graphs.front())),
                                  std::make_shared<const DecisionGraphLayout>(std::move(layout).Value()), ComputeSha256(graphBytes)});
        }

        /** @brief Publishes only a project-local recovery slot under the shared writer's exact ownership rules. */
        Result<void> WriteDecisionRecovery(const DecisionRecoveryCapture &capture, DocumentRecoveryService &recovery,
                                           const CancellationToken &cancellation = {}) {
            try {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                auto payload = EncodeDecisionRecovery(capture);
                if (payload.HasError())
                    return Result<void>::Failure(payload.ErrorValue());
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

        /** @brief Projects one owner revision without borrowing its edit state across the I/O job. */
        DecisionRecoveryCapture CaptureDecisionRecovery(const DecisionGraphWorkspaceDocument &document, Assets::AssetId session,
                                                        std::size_t acceptedBytes, Assets::AssetSourceIdentity acceptedLayout,
                                                        std::shared_ptr<const DocumentRecoveryRecord> acceptedRecovery) {
            return {document.asset, document.identity, session,         document.revision,          {true, acceptedBytes, document.source},
                    acceptedLayout, document.graph,    document.layout, std::move(acceptedRecovery)};
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::InspectDecisionGraphRecovery */
    void EditorWorkspaceController::InspectDecisionGraphRecovery(DecisionGraphWorkspaceDocument &document) const {
        if (!document.editable || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid())
            return;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        auto observed = recovery.Inspect(document.asset, document.identity.key);
        if (observed.HasError()) {
            LOG_ERROR("editor.decision_recovery", "Decision recovery inspection failed: %s", observed.ErrorValue().message.c_str());
        } else if (observed.Value()) {
            document.recovery = std::make_shared<const DocumentRecoveryRecord>(std::move(observed).Value().value());
        }
    }

    /** @copydoc EditorWorkspaceController::ReadDecisionGraphRecovery */
    Result<DecisionGraphDocumentRoot> EditorWorkspaceController::ReadDecisionGraphRecovery(const DecisionGraphWorkspaceDocument &document,
                                                                                           const DecisionGraphDraft &draft) const {
        using Root = Result<DecisionGraphDocumentRoot>;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        const auto observed = recovery.Inspect(document.asset, document.identity.key);
        if (observed.HasError())
            return Root::Failure(observed.ErrorValue());
        const Assets::AssetSourceIdentity acceptedSource{true, draft.acceptedBytes, document.source};
        if (!observed.Value() || *observed.Value() != *document.recovery || observed.Value()->acceptedSource != acceptedSource)
            return Root::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto *record = m_assetRegistry.Find(document.asset);
        if (!record || record->sourcePath.String() != document.identity.key.source.Value())
            return Root::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto path = Foundation::Paths::Resolve(ProjectRoot(), record->sourcePath);
        const auto layout = CaptureDecisionLayout(ProjectRoot(), path, document.asset, document.graph->asset);
        if (layout.HasError())
            return Root::Failure(layout.ErrorValue());
        if (layout.Value().accepted != draft.acceptedLayout)
            return Root::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return DecodeDecisionRecovery(*observed.Value(), document, draft.acceptedLayout);
    }

    /** @copydoc EditorWorkspaceController::ObserveDecisionGraphRecovery */
    void EditorWorkspaceController::ObserveDecisionGraphRecovery() {
        if (!m_decisionGraphRecovery)
            return;
        const auto result = m_decisionGraphRecovery->job.Snapshot();
        if (!result || !result->terminalResult)
            return;
        const auto fence = m_decisionGraphRecovery->fence;
        m_decisionGraphRecovery.reset();
        if (result->state == JobState::Succeeded) {
            const auto draft = m_decisionGraphDrafts.find(fence.instance);
            if (draft != m_decisionGraphDrafts.end()) {
                draft->second.recoveredRevision = std::max(draft->second.recoveredRevision, fence.revision);
                draft->second.acceptedRecovery.reset();
            }
        } else if (result->state != JobState::Cancelled && result->terminalResult->error) {
            LOG_ERROR("editor.decision_recovery", "Decision recovery checkpoint failed: %s",
                      result->terminalResult->error->message.c_str());
        }
    }

    /** @copydoc EditorWorkspaceController::JoinDecisionGraphRecovery */
    void EditorWorkspaceController::JoinDecisionGraphRecovery(const std::optional<DocumentInstanceId> instance) {
        if (!m_decisionGraphRecovery || (instance && m_decisionGraphRecovery->fence.instance != *instance))
            return;
        static_cast<void>(m_decisionGraphRecovery->job.Wait());
        ObserveDecisionGraphRecovery();
    }

    /** @copydoc EditorWorkspaceController::ClearDecisionGraphRecovery */
    Result<void> EditorWorkspaceController::ClearDecisionGraphRecovery(const DecisionGraphWorkspaceDocument &document,
                                                                       DecisionGraphDraft &draft) {
        JoinDecisionGraphRecovery(document.identity.instance);
        if (!m_workspaceSession.IsValid() || !m_mutations || !m_durableFiles || (draft.recoveredRevision == 0 && !draft.acceptedRecovery))
            return Result<void>::Success();
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        const auto observed = recovery.Inspect(document.asset, document.identity.key);
        if (observed.HasError())
            return Result<void>::Failure(observed.ErrorValue());
        const auto &record = observed.Value();
        const bool owned = record && record->sessionOwner == m_workspaceSession &&
                           record->sessionGeneration == document.identity.instance.Value() && record->revision == draft.recoveredRevision;
        const bool accepted = record && draft.acceptedRecovery && *record == *draft.acceptedRecovery;
        if (owned || accepted) {
            if (auto discarded = recovery.Discard(*record); discarded.HasError())
                return discarded;
        }
        draft.recoveredRevision = 0;
        draft.acceptedRecovery.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::UpdateDecisionGraphAutosave */
    void EditorWorkspaceController::UpdateDecisionGraphAutosave(const float seconds, const int minutes) {
        ObserveDecisionGraphRecovery();
        if (m_decisionAutosaveDiscarded || !m_sourceRecoveryJobs || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid() ||
            minutes <= 0 || seconds <= 0.0F || !std::isfinite(seconds))
            return;
        m_decisionAutosaveSeconds += seconds;
        if (m_decisionRecoveryQueue.empty() && m_decisionAutosaveSeconds >= static_cast<float>(minutes) * 60.0F) {
            m_decisionAutosaveSeconds = 0.0F;
            for (const auto &document : m_viewModel.decisionGraphDocuments)
                if (document.dirty)
                    m_decisionRecoveryQueue.push_back(document.identity.instance);
        }
        if (m_decisionGraphRecovery || m_decisionRecoveryQueue.empty())
            return;
        const auto instance = m_decisionRecoveryQueue.back();
        m_decisionRecoveryQueue.pop_back();
        const auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [instance](const auto &entry) {
            return entry.identity.instance == instance;
        });
        const auto draft = m_decisionGraphDrafts.find(instance);
        if (document == documents.end() || !document->dirty || draft == m_decisionGraphDrafts.end() ||
            draft->second.recoveredRevision >= document->revision)
            return;
        const auto capture = CaptureDecisionRecovery(*document, m_workspaceSession, draft->second.acceptedBytes,
                                                     draft->second.acceptedLayout, draft->second.acceptedRecovery);
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                            [root = ProjectRoot(), mutations = m_mutations, files = m_durableFiles,
                                                             capture](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files);
            return WriteDecisionRecovery(capture, recovery, cancellation);
        });
        if (submitted.HasError()) {
            LOG_ERROR("editor.decision_recovery", "Decision recovery scheduling failed: %s", submitted.ErrorValue().message.c_str());
            return;
        }
        m_decisionGraphRecovery.emplace(DecisionRecoveryWork{{instance, capture.revision}, std::move(submitted).Value()});
    }

    /** @copydoc EditorWorkspaceController::FlushDecisionGraphAutosave */
    void EditorWorkspaceController::FlushDecisionGraphAutosave() {
        JoinDecisionGraphRecovery();
        m_decisionRecoveryQueue.clear();
        m_decisionAutosaveSeconds = 0.0F;
        if (m_decisionAutosaveDiscarded || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid())
            return;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        for (const auto &document : m_viewModel.decisionGraphDocuments) {
            const auto draft = m_decisionGraphDrafts.find(document.identity.instance);
            if (!document.dirty || draft == m_decisionGraphDrafts.end() || draft->second.recoveredRevision >= document.revision)
                continue;
            const auto capture = CaptureDecisionRecovery(document, m_workspaceSession, draft->second.acceptedBytes,
                                                         draft->second.acceptedLayout, draft->second.acceptedRecovery);
            if (const auto saved = WriteDecisionRecovery(capture, recovery); saved.HasError())
                LOG_ERROR("editor.decision_recovery", "Decision recovery checkpoint failed: %s", saved.ErrorValue().message.c_str());
            else {
                draft->second.recoveredRevision = document.revision;
                draft->second.acceptedRecovery.reset();
            }
        }
    }

    /** @copydoc EditorWorkspaceController::DiscardDecisionGraphAutosaves */
    Result<void> EditorWorkspaceController::DiscardDecisionGraphAutosaves() {
        JoinDecisionGraphRecovery();
        for (const auto &document : m_viewModel.decisionGraphDocuments) {
            const auto draft = m_decisionGraphDrafts.find(document.identity.instance);
            if (draft != m_decisionGraphDrafts.end()) {
                if (auto cleared = ClearDecisionGraphRecovery(document, draft->second); cleared.HasError())
                    return cleared;
            }
        }
        m_decisionRecoveryQueue.clear();
        m_decisionAutosaveDiscarded = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
