#pragma once

/** @file EditorLocalAgentContext.h @brief Explicitly reviewed active editor source evidence. */

#include "AgentConversationStore.h"
#include "AgentTurn.h"
#include "Horo/Editor/AgentEditorDiagnosticsSnapshot.h"
#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/BuildOutputStore.h"

namespace Horo::Security {
    class SecureRandomSource;
}  // namespace Horo::Security

namespace Horo::Editor {
    /** @brief Owns one source preview and its exclusive policy until the approved request moves to the worker.
     * @details Approval runs on the editor lane. The worker observes immutable evidence and revocation witnesses,
     * and rechecks saved file revisions through the rooted reader. No editor-thread callback is retained.
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

        /** @brief Captures explicitly selected open files in selection order for a single consent review. */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareOpenFiles(std::vector<SourceDocumentSnapshot> sources,
                                                                              std::string projectIdentity,
                                                                              std::uint64_t workspaceGeneration, CancellationToken lifetime,
                                                                              std::uint64_t requestId);

        /** @brief Captures explicitly selected saved project text on a worker without opening a Source tab.
         * @param files Retained read-only authority for the current workspace.
         * @param path Exact portable project-relative path supplied by the user.
         * @param project Workspace reader identity, generation, lifetime and preview deadline.
         * @param requestId Exact upcoming local turn.
         * @param stop Cancellation for the capture operation only.
         * @return Owned redacted preview or a typed error before any provider access.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareSavedFile(std::shared_ptr<const Platform::IProjectReadFiles> files,
                                                                              std::string_view path, Platform::ProjectReadContext project,
                                                                              std::uint64_t requestId, std::stop_token stop = {});

        /** @brief Prepares only the bounded, already-correlated build messages selected by the workspace.
         * @details The tail must have been acquired from the project's exact active gameplay operation.
         * The worker receives a revocation witness and redacted bytes, never the process output query.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareBuildOutput(BuildOutputOperationTail tail, std::string projectIdentity,
                                                                                std::uint64_t workspaceGeneration,
                                                                                CancellationToken lifetime, std::uint64_t requestId);

        /** @brief Reads the selected build's diagnostic capture through the authenticated MCP worker route.
         * @param tail Owned exact-operation diagnostic capture.
         * @param project Project generation, cancellation and preview deadline.
         * @param requestId Exact upcoming local turn.
         * @param entropy Host entropy for the private query session.
         * @param stop Stop condition for the query worker.
         * @return Redacted review or typed admission or read failure.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareBuildDiagnostics(BuildOutputOperationTail tail,
                                                                                     Platform::ProjectReadContext project,
                                                                                     std::uint64_t requestId,
                                                                                     std::shared_ptr<Security::SecureRandomSource> entropy,
                                                                                     std::stop_token stop = {});
        /** @brief Reads the selected editor document's Problems through the private diagnostic query.
         * @param snapshot Bounded owner-issued findings and freshness token. @param project Current workspace fence.
         * @param requestId Exact upcoming local turn. @param entropy Private session credential entropy.
         * @param stop Worker cancellation. @return Owned redacted review or a typed failure.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareEditorDiagnostics(AgentEditorDiagnosticsSnapshot snapshot,
                                                                                      Platform::ProjectReadContext project,
                                                                                      std::uint64_t requestId,
                                                                                      std::shared_ptr<Security::SecureRandomSource> entropy,
                                                                                      std::stop_token stop);

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

        /** @brief Prepares a note or report explicitly copied from the clipboard by the user.
         * @param kind Note or Report chosen in the panel. @param text Borrowed complete clipboard text.
         * @param projectIdentity Current project identity. @param workspaceGeneration Nonzero attachment generation.
         * @param lifetime Workspace revocation witness. @param requestId Exact upcoming local turn.
         * @return An owned redacted preview or a typed failure. No clipboard borrow survives capture.
         */
        [[nodiscard]] static Result<EditorLocalAgentContext> PrepareClipboardAttachment(Agent::AgentContextAttachmentKind kind,
                                                                                        std::string_view text, std::string projectIdentity,
                                                                                        std::uint64_t workspaceGeneration,
                                                                                        CancellationToken lifetime,
                                                                                        std::uint64_t requestId);

        /** @brief Observe the exact bytes to present before asking for consent. */
        [[nodiscard]] std::span<const Agent::AgentContextPreviewItem> Items() const noexcept {
            return preview_.Items();
        }

        /** @brief Check that this same edition of the same document is still selected in the workspace. */
        [[nodiscard]] bool Matches(const SourceDocumentSnapshot &active) const noexcept;
        /** @brief Checks that the exact selected edition is still present among the open Source tabs. */
        [[nodiscard]] bool MatchesOpenSource(std::span<const SourceDocumentSnapshot> open) const noexcept;

        /** @brief Reports whether the preview requires an already-open Source tab to stay present. */
        [[nodiscard]] bool RequiresOpenSource() const noexcept {
            return source_.Revision() != 0 || !openFiles_.empty();
        }

        /** @brief Returns exact whole, unredacted files offered to the model. */
        [[nodiscard]] std::vector<SourceDocumentSnapshot> PatchSources() const;

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

        struct ViewportChoice {
            bool hasSelection{};
            AgentViewportComponents components{};
        };

        /** @brief Returns the disclosure scope only for a numeric viewport review. */
        [[nodiscard]] std::optional<ViewportChoice> Viewport() const noexcept {
            return viewport_;
        }

        /** @brief Identifies a preview of saved bytes captured through the workspace reader. */
        [[nodiscard]] bool IsSavedFile() const noexcept {
            return static_cast<bool>(savedFiles_);
        }

        /** @brief Reports the captured explicit selection, when reviewing a range. */
        [[nodiscard]] std::optional<SourceContextRange> Selection() const noexcept {
            return selection_;
        }

        /** @brief Record consent only after the exact preview is shown and the user explicitly approves it. */
        [[nodiscard]] bool Approve();

        /** @brief On the inference worker, recheck saved file bytes and borrow the context for the synchronous turn.
         * @param stop Cancellation for the inference worker. @return Owned-context borrow, denied if saved evidence changed.
         */
        [[nodiscard]] Agent::AgentTurnContext Dispatch(std::stop_token stop = {});

    private:
        /** @brief Admits the exact owned page and retains its producer witness for provider dispatch. */
        [[nodiscard]] static Result<EditorLocalAgentContext> CaptureDiagnosticPage(
            const Result<Application::CodeQueryPage> &page, std::function<bool()> current, Platform::ProjectReadContext project,
            std::uint64_t requestId, std::optional<OperationId> operation, Agent::AgentHistoryReference reference);
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
        std::vector<SourceDocumentSnapshot> openFiles_;
        Platform::ProjectReadContext project_;
        std::unique_ptr<Security::AgentPolicy> policy_;
        Agent::AgentContextPreview preview_;
        std::function<bool(const Agent::AgentContextProvenance &)> current_;
        std::optional<SourceContextRange> selection_;
        std::optional<OperationId> buildOperation_;
        std::optional<ViewportChoice> viewport_;
        Agent::AgentHistoryReference historyReference_;
        std::shared_ptr<const Platform::IProjectReadFiles> savedFiles_;
    };
}  // namespace Horo::Editor
