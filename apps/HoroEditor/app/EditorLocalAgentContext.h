#pragma once

/** @file EditorLocalAgentContext.h @brief Explicitly reviewed active editor source evidence. */

#include "AgentTurn.h"
#include "Horo/Editor/SourceDocumentService.h"

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
         * @return Bounded redacted context preview, or a typed denial before any provider access.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> Prepare(SourceDocumentSnapshot source, std::string projectIdentity,
                                                                     std::uint64_t workspaceGeneration, CancellationToken lifetime,
                                                                     std::uint64_t requestId);

        /** @brief Observe the exact bytes to present before asking for consent. */
        [[nodiscard]] std::span<const Agent::AgentContextPreviewItem> Items() const noexcept {
            return preview_.Items();
        }

        /** @brief Check that this same edition of the same document is still selected in the workspace. */
        [[nodiscard]] bool Matches(const SourceDocumentSnapshot &active) const noexcept;

        /** @brief Record consent only after the exact preview is shown and the user explicitly approves it. */
        [[nodiscard]] bool Approve();

        /** @brief Borrow the exclusive prepared context until the synchronous worker turn returns. */
        [[nodiscard]] Agent::AgentTurnContext Dispatch() noexcept;

    private:
        EditorLocalAgentContext(SourceDocumentSnapshot source, Platform::ProjectReadContext project,
                                std::unique_ptr<Security::AgentPolicy> policy, Agent::AgentContextPreview preview,
                                std::function<bool(const Agent::AgentContextProvenance &)> current);

        SourceDocumentSnapshot source_;
        Platform::ProjectReadContext project_;
        std::unique_ptr<Security::AgentPolicy> policy_;
        Agent::AgentContextPreview preview_;
        std::function<bool(const Agent::AgentContextProvenance &)> current_;
    };
}  // namespace Horo::Editor
