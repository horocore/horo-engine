#pragma once

#include "AgentHistoryReview.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Owner-lane view and exact approvals for one local panel's project history. */
    class EditorLocalAgentHistory final : private LocalizedGui {
    public:
        /** @brief Builds an idle view, borrowing the application's I/O worker and filesystem. */
        EditorLocalAgentHistory(const JobSystem &jobs, DurableFileSystem &files);

        /** @brief Defers the captured route binding until the editor's post-activation pump. */
        void Attach(Agent::AgentHistoryProject project);
        /** @brief Drops reviews, management drafts and all projections of the previous project. */
        void Detach();
        /** @brief Advances the worker's publication. Returns true only after the recoverable prompt is durable. */
        [[nodiscard]] bool Poll();
        /** @brief Returns the current project identity for a newly submitted plain text turn. */
        [[nodiscard]] Agent::AgentHistoryProjectRevision Project() const noexcept;
        /** @brief Prepares the exact redacted pair for a subsequent explicit save. */
        void Review(Agent::AgentHistoryProjectRevision project, std::string_view prompt, std::string_view answer,
                    Agent::AgentHistoryUsage usage);
        /** @brief Offers only the completed turn's numeric reference for separate history approval. */
        void ReviewReference(Agent::AgentHistoryProjectRevision project, Agent::AgentHistoryReference reference);
        /** @brief Removes the previous unsaved review before a new request. */
        void ClearReview() noexcept;
        /** @brief Checks whether an explicitly reviewed recoverable ask can be offered. */
        [[nodiscard]] bool CanReviewRecovery() const noexcept;
        /** @brief Prepares a redacted prompt for consent before any checkpoint or inference starts. */
        [[nodiscard]] bool ReviewRecoveryPrompt(std::string_view prompt);
        /** @brief Reports whether the pending prompt still awaits approval or the checkpoint receipt. */
        [[nodiscard]] bool RecoveryStartPending() const noexcept;
        /** @brief Reports whether a durable prompt still owns a recoverable response. */
        [[nodiscard]] bool HasRecovery() const noexcept;
        /** @brief Captures a fresh, sanitized draft for review without persisting it. */
        void ReviewLiveDraft(const Agent::AgentHistoryLiveDraft &draft);
        /** @brief Prepares the visible terminal answer for a second, independent approval. */
        void ReviewRecoveryAnswer(std::string_view answer, Agent::AgentHistoryUsage usage);
        /** @brief Settles a failed, cancelled or skipped response with no output payload. */
        void FinishRecovery(Agent::AgentHistoryMessageState state);
        /** @brief Presents approved storage and bounded conversation management on the editor lane.
         * @param context Borrowed GUI services. @return True while the conversation name field owns text input.
         */
        [[nodiscard]] bool Draw(const EditorGuiContext &context);

    private:
        enum class Action {
            Save,
            Change,
            Export,
            StartRecovery,
            SaveDraft,
            FinishRecovery
        };

        struct Pending final {
            Agent::AgentHistoryOperationId id;
            Action action;
            std::uint64_t checkpointTime{};
        };

        struct Deletion final {
            Agent::AgentHistoryProjectRevision project;
            Agent::AgentConversationId conversation;
        };

        struct ReferenceReview final {
            Agent::AgentHistoryProjectRevision project;
            Agent::AgentHistoryReference reference;
        };

        struct Recovery final {
            Agent::AgentHistoryRecoveryTurn binding;
            std::uint64_t checkpointTime{};
            std::optional<Agent::AgentHistoryAnswerReview> answer;
            std::optional<Agent::AgentHistoryAnswerReview> draft;
            std::optional<Agent::AgentHistoryMessageState> pendingSettlement;
        };

        [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled = true) const;
        [[nodiscard]] bool Queue(const Result<Agent::AgentHistoryOperationId> &operation, Action action, std::uint64_t checkpointTime = 0);
        /** @brief Releases draft publication and advances a terminal outcome that arrived during the write. */
        void AdoptDraftReceipt(bool saved, std::uint64_t checkpointTime);
        void DrawRecovery(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        /** @brief Offers one frozen redacted streaming preview for explicit approval. */
        void DrawDraft(const ILocalizationService &locale, bool enabled);
        void DrawReview(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void DrawReferenceReview(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void DrawReference(const ILocalizationService &locale, Agent::AgentHistoryReference reference) const;
        /** @brief Displays present, bounded counts beside the text whose save the user approves. */
        void DrawTokenUsage(const ILocalizationService &locale, Agent::AgentHistoryUsage usage) const;
        [[nodiscard]] bool DrawManager(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view);
        [[nodiscard]] bool DrawActions(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view,
                                       const Agent::AgentConversation &conversation);
        void DrawDeletion(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void Select(const Agent::AgentConversation &conversation);

        Agent::AgentHistorySession history_;
        std::optional<Agent::AgentHistoryProject> attached_;
        std::optional<Agent::AgentHistoryExchangeReview> review_;
        std::optional<ReferenceReview> referenceReview_;
        std::optional<Agent::AgentHistoryPromptReview> recoveryPrompt_;
        std::optional<Recovery> recovery_;
        std::optional<Pending> pending_;
        std::optional<Deletion> deletion_;
        Agent::AgentConversationId selected_;
        std::string title_;
        std::optional<std::string> export_;
        const char *status_{};
        bool managing_{};
    };
}  // namespace Horo::Editor
