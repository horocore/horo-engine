#include "EditorLocalAgentHistory.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <chrono>

namespace Horo::Editor {
    using namespace Agent;

    namespace {
        /** @brief Only healthy and recoverable project images can accept writes or exports. */
        bool Available(const AgentHistorySessionView &view) {
            using enum AgentHistoryAvailability;
            return view.availability == Ready || view.availability == Recovered || view.availability == DurabilityUnconfirmed;
        }

        /** @brief Displays data without interpreting its bytes as widget labels or format specifiers. */
        void DrawText(std::string_view text) {
            if (text.empty())
                return;
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            ImGui::PopTextWrapPos();
        }

        /** @brief Returns a nonnegative host timestamp for a completed exchange. */
        std::uint64_t WallSeconds() {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
        }
    }  // namespace

    /** @copydoc EditorLocalAgentHistory::EditorLocalAgentHistory */
    EditorLocalAgentHistory::EditorLocalAgentHistory(const JobSystem &jobs, DurableFileSystem &files)
        : LocalizedGui("workspace.local_agent"), history_(jobs, files, RedactHistoryDisplayText) {}

    /** @copydoc EditorLocalAgentHistory::Attach */
    void EditorLocalAgentHistory::Attach(AgentHistoryProject project) {
        attached_ = std::move(project);
    }

    /** @copydoc EditorLocalAgentHistory::Detach */
    void EditorLocalAgentHistory::Detach() {
        attached_.reset();
        history_.SelectProject(std::nullopt);
        review_.reset();
        pending_.reset();
        deletion_.reset();
        selected_ = {};
        title_.clear();
        export_.reset();
        status_ = nullptr;
        managing_ = false;
    }

    /** @copydoc EditorLocalAgentHistory::Poll */
    void EditorLocalAgentHistory::Poll() {
        if (attached_) {
            history_.SelectProject(std::move(attached_));
            attached_.reset();
        }
        history_.Poll();
        auto completed = history_.TakeMutation();
        if (!completed || !pending_ || completed->operation != pending_->id || completed->project != Project())
            return;
        const auto action = pending_->action;
        pending_.reset();
        if (completed->error) {
            status_ = "history_write_error";
            return;
        }
        if (action == Action::Export) {
            export_ = std::move(completed->exportedText);
            status_ = export_ ? nullptr : "history_write_error";
        } else {
            status_ = action == Action::Save ? "history_saved" : "history_changed";
            if (action == Action::Change) {
                const auto view = history_.View();
                const auto selected = std::ranges::find(view.conversations, selected_, &AgentConversation::id);
                if (selected != view.conversations.end())
                    title_ = selected->title;
            }
        }
    }

    /** @copydoc EditorLocalAgentHistory::Project */
    AgentHistoryProjectRevision EditorLocalAgentHistory::Project() const noexcept {
        return history_.View().project;
    }

    /** @copydoc EditorLocalAgentHistory::Review */
    void EditorLocalAgentHistory::Review(AgentHistoryProjectRevision project, std::string_view prompt, std::string_view answer) {
        if (project != Project())
            return;
        auto prepared = AgentHistoryExchangeReview::Prepare(project, prompt, answer);
        if (prepared.HasValue())
            review_.emplace(std::move(prepared).Value());
    }

    /** @copydoc EditorLocalAgentHistory::ClearReview */
    void EditorLocalAgentHistory::ClearReview() noexcept {
        review_.reset();
    }

    bool EditorLocalAgentHistory::Button(const ILocalizationService &locale, const char *key, bool enabled) const {
        const auto label = LocalizedText(locale, key) + "##" + key;
        return Ui::Button({.label = label.c_str(), .enabled = enabled});
    }

    bool EditorLocalAgentHistory::Queue(Result<AgentHistoryOperationId> operation, Action action) {
        if (operation.HasError()) {
            status_ = "history_write_error";
            return false;
        }
        pending_ = Pending{operation.Value(), action};
        export_.reset();
        status_ = action == Action::Save ? "history_saving" : "history_working";
        return true;
    }

    void EditorLocalAgentHistory::DrawReview(const ILocalizationService &locale, const AgentHistorySessionView &view) {
        if (!review_)
            return;
        ImGui::TextUnformatted(LocalizedText(locale, "history_review").c_str());
        DrawText(review_->Prompt());
        DrawText(review_->Response());
        if (Button(locale, "history_save", Available(view) && !pending_) &&
            Queue(review_->SaveApproved(history_, LocalizedText(locale, "history_title"), WallSeconds()), Action::Save))
            review_.reset();
        if (view.phase == AgentHistorySessionPhase::Settled && !Available(view))
            ImGui::TextWrapped("%s", LocalizedText(locale, "history_unavailable").c_str());
    }

    void EditorLocalAgentHistory::Select(const AgentConversation &conversation) {
        selected_ = conversation.id;
        title_ = conversation.title;
        export_.reset();
    }

    void EditorLocalAgentHistory::DrawManager(const EditorGuiContext &context, const AgentHistorySessionView &view) {
        const auto &locale = context.localization;
        if (view.conversations.empty())
            return;
        if (Button(locale, "history_manage"))
            managing_ = !managing_;
        if (!managing_)
            return;
        const auto selected = std::ranges::find(view.conversations, selected_, &AgentConversation::id);
        if (selected == view.conversations.end())
            Select(view.conversations.back());
        int index = selected == view.conversations.end() ? static_cast<int>(view.conversations.size() - 1)
                                                         : static_cast<int>(selected - view.conversations.begin());
        const auto conversations = view.conversations;
        const Ui::ComboItemSource source{.label = [conversations](int item) {
            return conversations[static_cast<std::size_t>(item)].title.c_str();
        }};
        ImGui::BeginDisabled(pending_.has_value());
        if (Ui::ComboControl("##LocalHistoryConversation", &index, static_cast<int>(conversations.size()), source, context.theme.fonts))
            Select(conversations[static_cast<std::size_t>(index)]);
        ImGui::EndDisabled();
        const auto &conversation = conversations[static_cast<std::size_t>(index)];
        DrawActions(context, view, conversation);
        const auto messages = std::span{conversation.messages};
        if (messages.size() > 8)
            ImGui::TextUnformatted(LocalizedText(locale, "history_recent_messages").c_str());
        for (const auto &message : messages.last(std::min(messages.size(), std::size_t{8})))
            DrawText(message.text);
    }

    void EditorLocalAgentHistory::DrawActions(const EditorGuiContext &context, const AgentHistorySessionView &view,
                                              const AgentConversation &conversation) {
        const auto &locale = context.localization;
        const bool enabled = Available(view) && !pending_;
        ImGui::TextUnformatted(LocalizedText(locale, "history_name").c_str());
        ImGui::BeginDisabled(!enabled);
        static_cast<void>(Ui::InputTextControl("##LocalHistoryName", title_, AgentHistoryLimits::MaximumTitleBytes, context.theme.fonts));
        ImGui::EndDisabled();
        if (Button(locale, "history_rename", enabled && !title_.empty() && title_ != conversation.title))
            static_cast<void>(Queue(history_.Rename(view.project, conversation.id, title_), Action::Change));
        if (Button(locale, conversation.archived ? "history_restore" : "history_archive", enabled && !pending_))
            static_cast<void>(Queue(history_.SetArchived(view.project, conversation.id, !conversation.archived), Action::Change));
        if (Button(locale, "history_export", enabled && !pending_))
            static_cast<void>(Queue(history_.Export(view.project, conversation.id), Action::Export));
        if (Button(locale, "history_delete", enabled && !pending_)) {
            deletion_ = Deletion{view.project, conversation.id};
            const auto popup = LocalizedText(locale, "history_delete") + "###LocalHistoryDelete";
            ImGui::OpenPopup(popup.c_str());
        }
    }

    void EditorLocalAgentHistory::DrawDeletion(const ILocalizationService &locale, const AgentHistorySessionView &view) {
        const auto popup = LocalizedText(locale, "history_delete") + "###LocalHistoryDelete";
        if (!ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;
        const auto found =
            deletion_ ? std::ranges::find(view.conversations, deletion_->conversation, &AgentConversation::id) : view.conversations.end();
        if (found == view.conversations.end() || deletion_->project != view.project) {
            deletion_.reset();
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped("%s", LocalizedText(locale, "history_delete_confirm").c_str());
            DrawText(found->title);
            const bool cancelled = Button(locale, "history_cancel");
            ImGui::SameLine();
            const bool deleted = Button(locale, "history_delete", Available(view) && !pending_) &&
                                 Queue(history_.Delete(view.project, found->id), Action::Change);
            if (cancelled || deleted) {
                deletion_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    /** @copydoc EditorLocalAgentHistory::Draw */
    void EditorLocalAgentHistory::Draw(const EditorGuiContext &context) {
        const auto view = history_.View();
        DrawReview(context.localization, view);
        DrawManager(context, view);
        DrawDeletion(context.localization, view);
        if (managing_ && export_) {
            DrawText(*export_);
            if (Button(context.localization, "history_copy_export"))
                ImGui::SetClipboardText(export_->c_str());
        }
        if (status_)
            ImGui::TextWrapped("%s", LocalizedText(context.localization, status_).c_str());
    }
}  // namespace Horo::Editor
