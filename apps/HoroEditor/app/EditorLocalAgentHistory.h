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
        /** @brief Prepares the visible terminal answer for a second, independent approval. */
        void ReviewRecoveryAnswer(std::string_view answer, Agent::AgentHistoryUsage usage);
        /** @brief Settles a failed, cancelled or skipped response with no output payload. */
        void FinishRecovery(Agent::AgentHistoryMessageState state);
        /** @brief Presents approved storage and bounded conversation management on the editor lane. */
        void Draw(const EditorGuiContext &context);

    private:
        enum class Action {
            Save,
            Change,
            Export,
            StartRecovery,
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

        struct Recovery final {
            Agent::AgentHistoryRecoveryTurn binding;
            std::uint64_t checkpointTime{};
            std::optional<Agent::AgentHistoryAnswerReview> answer;
        };

        [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled = true) const;
        [[nodiscard]] bool Queue(const Result<Agent::AgentHistoryOperationId> &operation, Action action, std::uint64_t checkpointTime = 0);
        void DrawRecovery(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void DrawReview(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        /** @brief Displays present, bounded counts beside the text whose save the user approves. */
        void DrawTokenUsage(const ILocalizationService &locale, Agent::AgentHistoryUsage usage) const;
        void DrawManager(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view);
        void DrawActions(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view,
                         const Agent::AgentConversation &conversation);
        void DrawDeletion(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void Select(const Agent::AgentConversation &conversation);

        Agent::AgentHistorySession history_;
        std::optional<Agent::AgentHistoryProject> attached_;
        std::optional<Agent::AgentHistoryExchangeReview> review_;
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
