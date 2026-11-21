#include "DecisionGraphCanvasModel.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr auto RecoveryVersion = std::byte{1};
        constexpr DocumentRecoveryLimits RecoveryLimits{.maximumRecordBytes =
                                                            AI::DecisionCodecLimits{}.maximumBytes + MaximumDecisionLayoutBytes + 4096};
        constexpr std::size_t LegacyDigestOffset = 2;
        constexpr std::size_t LayoutSizeBytes = 8;
        constexpr std::size_t PayloadLengthBytes = 4;

        void AppendRecoveryInteger(std::vector<std::byte> &bytes, const std::uint64_t value, std::size_t width) {
            for (; width > 0; --width)
                bytes.push_back(static_cast<std::byte>((value >> ((width - 1) * 8)) & 0xff));
        }

        std::uint64_t ReadRecoveryInteger(const std::span<const std::byte> bytes) noexcept {
            std::uint64_t value{};
            for (const auto byte : bytes)
                value = (value << 8) | std::to_integer<unsigned>(byte);
            return value;
        }

        /** @brief Exact immutable checkpoint input; safe to retain across an IO job. */
        struct DecisionRecoveryCapture final {
            Assets::AssetId asset;
            DocumentIdentity document;
            Assets::AssetId session;
            std::uint64_t revision;
            Assets::AssetSourceIdentity acceptedSource;
            Assets::AssetSourceIdentity acceptedLayout;
            std::shared_ptr<const AI::DecisionGraphSource> graph;
            std::shared_ptr<const DecisionGraphLayout> layout;
            std::shared_ptr<const DocumentRecoveryRecord> acceptedCheckpoint;
        };

        DecisionRecoveryCapture CaptureDecisionRecovery(const DecisionGraphWorkspaceDocument &document, Assets::AssetId session,
                                                        std::size_t acceptedBytes, Assets::AssetSourceIdentity acceptedLayout,
                                                        std::shared_ptr<const DocumentRecoveryRecord> acceptedCheckpoint) {
            return {document.asset,
                    document.identity,
                    session,
                    document.revision,
                    {true, acceptedBytes, document.source},
                    acceptedLayout,
                    document.graph,
                    document.layout,
                    std::move(acceptedCheckpoint)};
        }

        // The payload joins the existing source codecs with the accepted layout fingerprint.
        Result<std::vector<std::byte>> EncodeDecisionRecovery(const DecisionRecoveryCapture &capture) {
            const auto &layoutBase = capture.acceptedLayout;
            using Encoded = Result<std::vector<std::byte>>;
            const auto graph = AI::DecisionPlanCodec::EncodeSource(std::span{capture.graph.get(), 1}, capture.graph->asset);
            if (graph.HasError())
                return Encoded::Failure(graph.ErrorValue());
            const auto layout = EncodeDecisionLayout(*capture.layout, capture.asset, capture.graph->asset);
            if (layout.HasError())
                return Encoded::Failure(layout.ErrorValue());
            const auto graphBytes = std::as_bytes(std::span{graph.Value()});
            const auto fingerprint = std::as_bytes(std::span{layoutBase.digest.bytes});
            const auto headerSize = LegacyDigestOffset + LayoutSizeBytes + fingerprint.size() + PayloadLengthBytes;
            if (graphBytes.size() + layout.Value().size() > RecoveryLimits.maximumRecordBytes - headerSize)
                return Encoded::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            std::vector<std::byte> payload;
            payload.reserve(headerSize + graphBytes.size() + layout.Value().size());
            payload.push_back(RecoveryVersion);
            payload.push_back(layoutBase.exists ? std::byte{1} : std::byte{0});
            AppendRecoveryInteger(payload, layoutBase.bytes, LayoutSizeBytes);
            payload.insert(payload.end(), fingerprint.begin(), fingerprint.end());
            AppendRecoveryInteger(payload, graphBytes.size(), PayloadLengthBytes);
            payload.insert(payload.end(), graphBytes.begin(), graphBytes.end());
            payload.insert(payload.end(), layout.Value().begin(), layout.Value().end());
            return Encoded::Success(std::move(payload));
        }

        Result<void> PublishDecisionRecoveryCheckpoint(const DecisionRecoveryCapture &capture, DocumentRecoveryService &recovery,
                                                       const CancellationToken &cancellation = {}) {
            using Written = Result<void>;
            try {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                auto payload = EncodeDecisionRecovery(capture);
                if (payload.HasError())
                    return Written::Failure(std::move(payload).ErrorValue());
                const auto now =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const DocumentRecoveryRecord record{.asset = capture.asset,
                                                    .document = capture.document.key,
                                                    .sessionOwner = capture.session,
                                                    .sessionGeneration = capture.document.instance.Value(),
                                                    .revision = capture.revision,
                                                    .capturedAtUnixSeconds = static_cast<std::uint64_t>(now),
                                                    .acceptedSource = capture.acceptedSource,
                                                    .state = ComputeSha256(payload.Value()),
                                                    .payload = std::move(payload).Value()};
                return capture.acceptedCheckpoint ? recovery.ReplaceAccepted(record, *capture.acceptedCheckpoint, cancellation)
                                                  : recovery.Write(record, cancellation);
            } catch (const std::bad_alloc &) {
                return Written::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            }
        }

        struct DecisionRecoverySections final {
            std::span<const std::byte> graph;
            std::span<const std::byte> layout;
        };

        /** @brief Accepts both version-one encodings without relaxing the exact layout identity fence. */
        Result<DecisionRecoverySections> SplitDecisionRecovery(const std::span<const std::byte> bytes,
                                                               const Assets::AssetSourceIdentity &layoutBase) {
            using Sections = Result<DecisionRecoverySections>;
            const auto fingerprint = std::as_bytes(std::span{layoutBase.digest.bytes});
            const auto legacyLengthOffset = LegacyDigestOffset + fingerprint.size();
            if (bytes.size() <= legacyLengthOffset + PayloadLengthBytes || bytes.front() != RecoveryVersion)
                return Sections::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            if (bytes[1] != (layoutBase.exists ? std::byte{1} : std::byte{0}))
                return Sections::Failure(MakeError(Assets::AssetSourceErrors::Conflict));

            const auto canonicalLengthOffset = legacyLengthOffset + LayoutSizeBytes;
            const bool graphFirst =
                bytes.size() > canonicalLengthOffset + PayloadLengthBytes &&
                ReadRecoveryInteger(bytes.subspan(LegacyDigestOffset, LayoutSizeBytes)) == layoutBase.bytes &&
                std::ranges::equal(bytes.subspan(LegacyDigestOffset + LayoutSizeBytes, fingerprint.size()), fingerprint);
            if (!graphFirst && !std::ranges::equal(bytes.subspan(LegacyDigestOffset, fingerprint.size()), fingerprint))
                return Sections::Failure(MakeError(Assets::AssetSourceErrors::Conflict));

            const auto lengthOffset = graphFirst ? canonicalLengthOffset : legacyLengthOffset;
            const auto firstSize = static_cast<std::size_t>(ReadRecoveryInteger(bytes.subspan(lengthOffset, PayloadLengthBytes)));
            const auto data = bytes.subspan(lengthOffset + PayloadLengthBytes);
            if (firstSize == 0 || firstSize >= data.size())
                return Sections::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto first = data.first(firstSize);
            const auto second = data.subspan(firstSize);
            const DecisionRecoverySections sections =
                graphFirst ? DecisionRecoverySections{first, second} : DecisionRecoverySections{second, first};
            if (sections.graph.size() > AI::DecisionCodecLimits{}.maximumBytes || sections.layout.size() > MaximumDecisionLayoutBytes)
                return Sections::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return Sections::Success(sections);
        }

        Result<DecisionGraphDocumentRoot> DecodeDecisionRecovery(const DecisionGraphWorkspaceDocument &document,
                                                                 const DocumentRecoveryRecord &record,
                                                                 const Assets::AssetSourceIdentity &layoutBase) {
            using Decoded = Result<DecisionGraphDocumentRoot>;
            const std::span<const std::byte> bytes{record.payload};
            if (ComputeSha256(bytes) != record.state)
                return Decoded::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            const auto sections = SplitDecisionRecovery(bytes, layoutBase);
            if (sections.HasError())
                return Decoded::Failure(sections.ErrorValue());
            auto layout = DecodeDecisionLayout(sections.Value().layout, document.asset, document.graph->asset);
            if (layout.HasError())
                return Decoded::Failure(std::move(layout).ErrorValue());
            const auto graph = sections.Value().graph;
            AI::DecisionGraphAssetId root;
            auto sources = AI::DecisionPlanCodec::DecodeSource({std::bit_cast<const std::uint8_t *>(graph.data()), graph.size()}, root);
            if (sources.HasError())
                return Decoded::Failure(std::move(sources).ErrorValue());
            if (root != document.graph->asset || sources.Value().size() != 1 || sources.Value().front().asset != root ||
                sources.Value().front().body.index() != document.graph->body.index())
                return Decoded::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            return Decoded::Success({std::make_shared<const AI::DecisionGraphSource>(std::move(sources.Value().front())),
                                     std::make_shared<const DecisionGraphLayout>(std::move(layout).Value()), ComputeSha256(graph)});
        }

        Result<void> CheckDecisionRecoveryBase(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                               const DecisionGraphWorkspaceDocument &document,
                                               const Assets::AssetSourceIdentity &layoutBase) {
            using Checked = Result<void>;
            const auto *registered = registry.Find(document.asset);
            if (!registered || registered->sourcePath.String() != document.identity.key.source.Value())
                return Checked::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto maximum = AI::DecisionCodecLimits{}.maximumBytes;
            const auto graph =
                Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&document.asset, 1},
                                                                {.maximumSourceBytes = maximum, .maximumAssets = 1}, maximum);
            if (graph.HasError())
                return Checked::Failure(graph.ErrorValue());
            const auto &source = graph.Value().Sources().front();
            const Assets::AssetSourceIdentity sourceIdentity{true, source.bytes.size(), source.sourceDigest};
            const auto path = Foundation::Paths::Resolve(root, source.record.sourcePath);
            const auto layout = Assets::CaptureAssetSource(root, DecisionLayoutPath(path), MaximumDecisionLayoutBytes);
            if (layout.HasError())
                return Checked::Failure(layout.ErrorValue());
            if (sourceIdentity != document.recovery->acceptedSource || layout.Value().identity != layoutBase)
                return Checked::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return graph.Value().VerifyUnchanged();
        }

        bool OwnsDecisionRecovery(const DocumentRecoveryRecord &record, Assets::AssetId session, DocumentInstanceId instance) {
            return record.sessionOwner == session && record.sessionGeneration == instance.Value();
        }
    }  // namespace

    void EditorWorkspaceController::ObserveDecisionGraphRecovery() {
        if (!m_decisionGraphRecovery)
            return;
        const auto terminal = m_decisionGraphRecovery->job.Snapshot();
        if (!terminal || !terminal->terminalResult)
            return;
        auto work = std::move(*m_decisionGraphRecovery);
        m_decisionGraphRecovery.reset();
        if (terminal->state != JobState::Succeeded) {
            if (terminal->state != JobState::Cancelled && terminal->terminalResult->error)
                LOG_ERROR("editor.decision_recovery", "Decision recovery checkpoint failed: %s",
                          terminal->terminalResult->error->message.c_str());
            return;
        }
        const auto draft = m_decisionGraphDrafts.find(work.fence.instance);
        if (draft == m_decisionGraphDrafts.end())
            return;
        draft->second.recoveryRevision = std::max(draft->second.recoveryRevision, work.fence.revision);
        if (draft->second.acceptedRecovery == work.acceptedCheckpoint)
            draft->second.acceptedRecovery.reset();
    }

    void EditorWorkspaceController::JoinDecisionGraphRecovery(const std::optional<DocumentInstanceId> instance) {
        if (!m_decisionGraphRecovery || (instance && m_decisionGraphRecovery->fence.instance != *instance))
            return;
        static_cast<void>(m_decisionGraphRecovery->job.Wait());
        ObserveDecisionGraphRecovery();
    }

    void EditorWorkspaceController::UpdateDecisionRecovery(const float elapsedSeconds, const int intervalMinutes) {
        ObserveDecisionGraphRecovery();
        if (m_decisionAutosaveDiscarded || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid() || intervalMinutes <= 0 ||
            !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0)
            return;
        m_decisionRecoverySeconds += elapsedSeconds;
        const bool due = m_decisionRecoverySeconds >= static_cast<float>(intervalMinutes) * 60.0F;
        if (!m_sourceRecoveryJobs) {
            if (due)
                FlushDecisionRecovery();
            return;
        }
        if (due && m_decisionRecoveryQueue.empty()) {
            m_decisionRecoverySeconds = 0;
            for (const auto &document : m_viewModel.decisionGraphDocuments)
                if (document.dirty)
                    m_decisionRecoveryQueue.push_back(document.identity.instance);
        }
        if (!m_decisionGraphRecovery && !m_decisionRecoveryQueue.empty())
            StartQueuedDecisionRecovery();
    }

    void EditorWorkspaceController::StartQueuedDecisionRecovery() {
        const auto instance = m_decisionRecoveryQueue.back();
        m_decisionRecoveryQueue.pop_back();
        const auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [instance](const auto &entry) {
            return entry.identity.instance == instance;
        });
        const auto draft = m_decisionGraphDrafts.find(instance);
        if (document == documents.end() || !document->dirty || document->recovery || draft == m_decisionGraphDrafts.end() ||
            draft->second.recoveryRevision >= document->revision || HasPendingBlackboardReferenceEdit(instance))
            return;
        const auto capture = CaptureDecisionRecovery(*document, m_workspaceSession, draft->second.acceptedBytes,
                                                     draft->second.acceptedLayout, draft->second.acceptedRecovery);
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                            [root = ProjectRoot(), mutations = m_mutations, files = m_durableFiles,
                                                             capture](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files, RecoveryLimits);
            return PublishDecisionRecoveryCheckpoint(capture, recovery, cancellation);
        });
        if (submitted.HasError()) {
            LOG_ERROR("editor.decision_recovery", "Decision recovery scheduling failed: %s", submitted.ErrorValue().message.c_str());
            return;
        }
        draft->second.recoveryAttempted = true;
        m_decisionGraphRecovery.emplace(
            DecisionRecoveryWork{{instance, capture.revision}, std::move(submitted).Value(), capture.acceptedCheckpoint});
    }

    void EditorWorkspaceController::FlushDecisionRecovery() {
        JoinDecisionGraphRecovery();
        m_decisionRecoveryQueue.clear();
        m_decisionRecoverySeconds = 0;
        if (m_decisionAutosaveDiscarded || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid())
            return;
        for (auto &document : m_viewModel.decisionGraphDocuments) {
            const auto draft = m_decisionGraphDrafts.find(document.identity.instance);
            if (draft == m_decisionGraphDrafts.end() || HasPendingBlackboardReferenceEdit(document.identity.instance))
                continue;
            const auto result =
                document.dirty ? WriteDecisionRecovery(document, draft->second) : DiscardOwnedDecisionRecovery(document, draft->second);
            if (result.HasError())
                LOG_ERROR("editor.decision_recovery", "Decision recovery checkpoint failed: %s", result.ErrorValue().message.c_str());
        }
    }

    Result<void> EditorWorkspaceController::DiscardDecisionGraphAutosaves() {
        JoinDecisionGraphRecovery();
        for (const auto &document : m_viewModel.decisionGraphDocuments) {
            const auto draft = m_decisionGraphDrafts.find(document.identity.instance);
            if (draft != m_decisionGraphDrafts.end()) {
                if (auto cleared = DiscardOwnedDecisionRecovery(document, draft->second); cleared.HasError())
                    return cleared;
            }
        }
        m_decisionRecoveryQueue.clear();
        m_decisionAutosaveDiscarded = true;
        return Result<void>::Success();
    }

    Result<void> EditorWorkspaceController::WriteDecisionRecovery(const DecisionGraphWorkspaceDocument &document,
                                                                  DecisionGraphDraft &draft) {
        if (document.recovery || draft.recoveryRevision >= document.revision)
            return Result<void>::Success();
        const auto capture =
            CaptureDecisionRecovery(document, m_workspaceSession, draft.acceptedBytes, draft.acceptedLayout, draft.acceptedRecovery);
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles, RecoveryLimits);
        draft.recoveryAttempted = true;
        auto result = PublishDecisionRecoveryCheckpoint(capture, recovery);
        if (result.HasValue()) {
            draft.recoveryRevision = document.revision;
            draft.acceptedRecovery.reset();
        }
        return result;
    }

    void EditorWorkspaceController::InspectDecisionRecovery(DecisionGraphWorkspaceDocument &document) {
        if (!document.editable || !m_mutations || !m_durableFiles || !m_workspaceSession.IsValid())
            return;
        const DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles, RecoveryLimits);
        auto existing = recovery.Inspect(document.asset, document.identity.key);
        if (existing.HasError()) {
            LOG_ERROR("editor.decision_recovery", "Decision recovery read failed: %s", existing.ErrorValue().message.c_str());
            return;
        }
        if (!existing.Value() || OwnsDecisionRecovery(*existing.Value(), m_workspaceSession, document.identity.instance))
            document.recovery.reset();
        else if (!document.recovery || *document.recovery != *existing.Value())
            document.recovery = std::make_shared<const DocumentRecoveryRecord>(std::move(*existing.Value()));
    }

    void EditorWorkspaceController::ProcessDecisionRecovery(const EditorWorkspaceViewCommandData &command) {
        if (!command.decisionGraphFence || !command.decisionGraphRecovery || !m_mutations || !m_durableFiles)
            return;
        const auto fence = *command.decisionGraphFence;
        if (m_viewModel.workspacePanelHost.ActiveDocument() != fence.instance)
            return;
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [fence](const auto &entry) {
            return entry.identity.instance == fence.instance && entry.revision == fence.revision;
        });
        if (document == documents.end() || document->recovery != command.decisionGraphRecovery)
            return;
        try {
            JoinDecisionGraphRecovery(fence.instance);
            const auto draft = m_decisionGraphDrafts.find(fence.instance);
            DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles, RecoveryLimits);
            const auto result = [&]() -> Result<void> {
                if (command.command == EditorWorkspaceViewCommand::DiscardDecisionGraphRecovery)
                    return recovery.Discard(*document->recovery);
                if (draft == m_decisionGraphDrafts.end() || HasPendingBlackboardReferenceEdit(fence.instance))
                    return Result<void>::Failure(MakeError(Application::DecisionCookErrors::Stale));
                return RestoreDecisionRecovery(*document, draft->second);
            }();
            if (result.HasValue()) {
                document->recovery.reset();
                document->diagnostic.reset();
            } else {
                document->diagnostic = result.ErrorValue();
                PublishDecisionGraphDiagnostics(*document->diagnostic);
            }
        } catch (const std::bad_alloc &) {
            document->diagnostic = MakeError(Assets::AssetSourceErrors::Limit);
        }
    }

    Result<void> EditorWorkspaceController::RestoreDecisionRecovery(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft) {
        using Restored = Result<void>;
        const auto &record = *document.recovery;
        if (document.dirty || document.conflict ||
            record.acceptedSource != Assets::AssetSourceIdentity{true, draft.acceptedBytes, document.source})
            return Restored::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto root = ProjectRoot();
        DocumentRecoveryService recovery(root, *m_mutations, *m_durableFiles, RecoveryLimits);
        const auto current = recovery.Inspect(document.asset, document.identity.key);
        if (current.HasError())
            return Restored::Failure(current.ErrorValue());
        if (!current.Value() || *current.Value() != record)
            return Restored::Failure(MakeError(Application::DecisionCookErrors::Stale));
        auto decoded = DecodeDecisionRecovery(document, record, draft.acceptedLayout);
        if (decoded.HasError())
            return Restored::Failure(std::move(decoded).ErrorValue());
        const auto &candidate = decoded.Value();
        if (auto validated = ValidateDecisionDraftRoot(*candidate.graph, document); validated.HasError())
            return validated;
        const auto canvas = ProjectDecisionCanvas(*candidate.graph, {document.identity.instance, document.revision}, document.editable);
        if (PruneDecisionLayout(*candidate.layout, canvas) != *candidate.layout ||
            (!document.layoutWritable && *candidate.layout != *document.layout))
            return Restored::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        if (auto checked = CheckDecisionRecoveryBase(root, registry, document, draft.acceptedLayout); checked.HasError())
            return checked;
        if (draft.currentDigest == candidate.digest && *document.layout == *candidate.layout)
            return recovery.Discard(record);
        auto prepared = draft.history.PrepareChange(CaptureDecisionHistoryFence(document, draft), std::move(decoded).Value(),
                                                    draft.acceptedBytes + record.payload.size());
        if (prepared.HasError())
            return Restored::Failure(std::move(prepared).ErrorValue());
        auto reservation = std::move(prepared).Value();
        if (!PublishDecisionGraphChange(document, draft, reservation))
            return Restored::Failure(MakeError(Application::DecisionCookErrors::Stale));
        draft.acceptedRecovery = document.recovery;
        ProjectDecisionGraphDraft(document, draft);
        return Restored::Success();
    }

    Result<void> EditorWorkspaceController::DiscardOwnedDecisionRecovery(const DecisionGraphWorkspaceDocument &document,
                                                                         DecisionGraphDraft &draft) {
        using Discarded = Result<void>;
        JoinDecisionGraphRecovery(document.identity.instance);
        if (!draft.recoveryAttempted && !draft.acceptedRecovery)
            return Discarded::Success();
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles, RecoveryLimits);
        const auto existing = recovery.Inspect(document.asset, document.identity.key);
        if (existing.HasError())
            return Discarded::Failure(existing.ErrorValue());
        if (existing.Value()) {
            const auto &record = *existing.Value();
            if (OwnsDecisionRecovery(record, m_workspaceSession, document.identity.instance) ||
                (draft.acceptedRecovery && record == *draft.acceptedRecovery)) {
                if (const auto discarded = recovery.Discard(record); discarded.HasError())
                    return discarded;
            }
        }
        draft.recoveryAttempted = false;
        draft.recoveryRevision = 0;
        draft.acceptedRecovery.reset();
        return Discarded::Success();
    }

    void EditorWorkspaceController::DiscardDecisionRecoveryAfterSave(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft) {
        if (const auto discarded = DiscardOwnedDecisionRecovery(document, draft); discarded.HasError())
            LOG_ERROR("editor.decision_recovery", "Saved Decision recovery cleanup failed: %s", discarded.ErrorValue().message.c_str());
    }
}  // namespace Horo::Editor
