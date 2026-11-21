#pragma once

/** @file EditorLocalAgentContext.h @brief Explicitly reviewed active editor source evidence. */

#include "AgentConversationStore.h"
#include "AgentTurn.h"
#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/BuildOutputStore.h"

namespace Horo::Editor {
    /** @brief Owns one source preview and its exclusive policy until the approved request moves to the worker.
     * @details The editor lane alone captures and approves. The worker observes only immutable source evidence and
     * revocation witnesses. No source service, widget, or editor-thread callback is retained by the request.
     */
    class EditorLocalAgentContext final {
    public:
        /** @brief Prepare evidence from the exact already-open source selected by the workspace.
         * @param source Immutable live source snapshot from the editor owner.
         * @param projectIdentity Committed project's stable identity.
         * @param workspaceGeneration Nonzero local attachment generation.
         * @param lifetime Revocable project attachment token.
         * @param requestId Exact host request identity reserved for the upcoming turn.
         * @param selection Explicit byte range from the active widget, or empty to review the complete file.
         * @return Bounded redacted context preview, or a typed denial before any provider access.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> Prepare(SourceDocumentSnapshot source, std::string projectIdentity,
                                                                     std::uint64_t workspaceGeneration, CancellationToken lifetime,
                                                                     std::uint64_t requestId,
                                                                     std::optional<SourceContextRange> selection = {});

        /** @brief Prepares only the bounded, already-correlated build messages selected by the workspace.
         * @details The tail must have been acquired from the project's exact active gameplay operation.
         * The worker receives a revocation witness and redacted bytes, never the process output query.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareBuildOutput(BuildOutputOperationTail tail, std::string projectIdentity,
                                                                                std::uint64_t workspaceGeneration,
                                                                                CancellationToken lifetime, std::uint64_t requestId);
        /** @brief Prepares the selected Scene IDs and exact revision from the existing scene owner.
         * @param selection Bounded snapshot and owner revocation witness.
         * @param projectIdentity Exact committed project identity.
         * @param workspaceGeneration Nonzero workspace attachment generation.
         * @param lifetime Workspace revocation witness.
         * @param requestId Exact next local turn.
         * @return Owned review or typed rejection of stale/malformed input.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareSceneSelection(const AgentSceneSelectionSnapshot &selection,
                                                                                   std::string projectIdentity,
                                                                                   std::uint64_t workspaceGeneration,
                                                                                   CancellationToken lifetime, std::uint64_t requestId);

        /** @brief Observe the exact bytes to present before asking for consent. */
        [[nodiscard]] std::span<const Agent::AgentContextPreviewItem> Items() const noexcept {
            return preview_.Items();
        }

        /** @brief Check that this same edition of the same document is still selected in the workspace. */
        [[nodiscard]] bool Matches(const SourceDocumentSnapshot &active) const noexcept;
        /** @brief Checks that the exact selected edition is still present among the open Source tabs. */
        [[nodiscard]] bool MatchesOpenSource(std::span<const SourceDocumentSnapshot> open) const noexcept;
        /** @brief Checks the captured authority without accessing any editor or output owner. */
        [[nodiscard]] bool IsCurrent() const;

        /** @brief Identifies the exact build operation when this is a build-output preview. */
        [[nodiscard]] std::optional<OperationId> BuildOperation() const noexcept {
            return buildOperation_;
        }

        /** @brief Returns the numeric source occurrence for optional history review. */
        [[nodiscard]] std::optional<Agent::AgentHistoryReference> HistoryReference() const noexcept {
            return historyReference_.identity ? std::optional{historyReference_} : std::nullopt;
        }

        /** @brief Reports whether the review holds only ordinary Scene selection evidence. */
        [[nodiscard]] bool IsSceneSelection() const noexcept {
            return !Items().empty() && Items().front().provenance.kind == Agent::AgentContextKind::Selection;
        }

        /** @brief Reports the captured explicit selection, when reviewing a range. */
        [[nodiscard]] std::optional<SourceContextRange> Selection() const noexcept {
            return selection_;
        }

        /** @brief Record consent only after the exact preview is shown and the user explicitly approves it. */
        [[nodiscard]] bool Approve();

        /** @brief Borrow the exclusive prepared context until the synchronous worker turn returns. */
        [[nodiscard]] Agent::AgentTurnContext Dispatch() noexcept;

    private:
        EditorLocalAgentContext(SourceDocumentSnapshot source, Platform::ProjectReadContext project,
                                std::unique_ptr<Security::AgentPolicy> policy, Agent::AgentContextPreview preview,
                                std::function<bool(const Agent::AgentContextProvenance &)> current,
                                std::optional<SourceContextRange> selection, std::optional<OperationId> buildOperation,
                                Agent::AgentHistoryReference historyReference);
        [[nodiscard]] static Result<EditorLocalAgentContext> Capture(const Agent::AgentContextSelection &selected,
                                                                     Agent::AgentContextReaders readers,
                                                                     Platform::ProjectReadContext project, std::uint64_t requestId,
                                                                     SourceDocumentSnapshot source, std::optional<SourceContextRange> range,
                                                                     std::optional<OperationId> buildOperation,
                                                                     Agent::AgentHistoryReference historyReference);

        SourceDocumentSnapshot source_;
        Platform::ProjectReadContext project_;
        std::unique_ptr<Security::AgentPolicy> policy_;
        Agent::AgentContextPreview preview_;
        std::function<bool(const Agent::AgentContextProvenance &)> current_;
        std::optional<SourceContextRange> selection_;
        std::optional<OperationId> buildOperation_;
        Agent::AgentHistoryReference historyReference_;
    };
}  // namespace Horo::Editor
