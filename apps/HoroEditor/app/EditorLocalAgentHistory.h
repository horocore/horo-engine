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
        /** @brief Advances the worker's immutable publication and consumes its exact receipt. */
        void Poll();
        /** @brief Returns the current project identity for a newly submitted plain text turn. */
        [[nodiscard]] Agent::AgentHistoryProjectRevision Project() const noexcept;
        /** @brief Prepares the exact redacted pair for a subsequent explicit save. */
        void Review(Agent::AgentHistoryProjectRevision project, std::string_view prompt, std::string_view answer);
        /** @brief Removes the previous unsaved review before a new request. */
        void ClearReview() noexcept;
        /** @brief Presents approved storage and bounded conversation management on the editor lane. */
        void Draw(const EditorGuiContext &context);

    private:
        enum class Action {
            Save,
            Change,
            Export
        };

        struct Pending final {
            Agent::AgentHistoryOperationId id;
            Action action;
        };

        struct Deletion final {
            Agent::AgentHistoryProjectRevision project;
            Agent::AgentConversationId conversation;
        };

        [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled = true) const;
        [[nodiscard]] bool Queue(Result<Agent::AgentHistoryOperationId> operation, Action action);
        void DrawReview(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void DrawManager(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view);
        void DrawActions(const EditorGuiContext &context, const Agent::AgentHistorySessionView &view,
                         const Agent::AgentConversation &conversation);
        void DrawDeletion(const ILocalizationService &locale, const Agent::AgentHistorySessionView &view);
        void Select(const Agent::AgentConversation &conversation);

        Agent::AgentHistorySession history_;
        std::optional<Agent::AgentHistoryProject> attached_;
        std::optional<Agent::AgentHistoryExchangeReview> review_;
        std::optional<Pending> pending_;
        std::optional<Deletion> deletion_;
        Agent::AgentConversationId selected_;
        std::string title_;
        std::optional<std::string> export_;
        const char *status_{};
        bool managing_{};
    };
}  // namespace Horo::Editor
