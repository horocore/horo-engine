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

        /** @brief Identifies a saved response even when no text has been approved. */
        const char *ResponseStateKey(const AgentHistoryMessageState state) {
            using enum AgentHistoryMessageState;
            switch (state) {
                case Streaming:
                    return "history_streaming";
                case Incomplete:
                    return "history_incomplete";
                case Cancelled:
                    return "cancelled";
                case Failed:
                    return "failed";
                case Complete:
                    return nullptr;
            }
            return nullptr;
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
        recoveryPrompt_.reset();
        recovery_.reset();
        pending_.reset();
        deletion_.reset();
        selected_ = {};
        title_.clear();
        export_.reset();
        status_ = nullptr;
        managing_ = false;
    }

    /** @copydoc EditorLocalAgentHistory::Poll */
    bool EditorLocalAgentHistory::Poll() {
        if (attached_) {
            history_.SelectProject(std::move(attached_));
            attached_.reset();
        }
        history_.Poll();
        auto completed = history_.TakeMutation();
        if (!completed || !pending_ || completed->operation != pending_->id || completed->project != Project())
            return false;
        const auto accepted = *pending_;
        const auto action = accepted.action;
        pending_.reset();
        if (completed->error) {
            status_ = "history_write_error";
            if (action == Action::FinishRecovery && recovery_ && !recovery_->answer)
                recovery_.reset();
            return false;
        }
        if (action == Action::StartRecovery) {
            if (!completed->started) {
                status_ = "history_write_error";
                return false;
            }
            recovery_.emplace(AgentHistoryRecoveryTurn{completed->project, *completed->started}, accepted.checkpointTime);
            status_ = "history_streaming";
            return true;
        }
        if (action == Action::FinishRecovery) {
            recovery_.reset();
            status_ = "history_saved";
            return false;
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
        return false;
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

    /** @copydoc EditorLocalAgentHistory::CanReviewRecovery */
    bool EditorLocalAgentHistory::CanReviewRecovery() const noexcept {
        return Available(history_.View()) && !pending_ && !recoveryPrompt_ && !recovery_;
    }

    /** @copydoc EditorLocalAgentHistory::ReviewRecoveryPrompt */
    bool EditorLocalAgentHistory::ReviewRecoveryPrompt(std::string_view prompt) {
        if (!CanReviewRecovery())
            return false;
        auto prepared = AgentHistoryPromptReview::Prepare(Project(), prompt);
        if (prepared.HasError()) {
            status_ = "history_write_error";
            return false;
        }
        review_.reset();
        recoveryPrompt_.emplace(std::move(prepared).Value());
        return true;
    }

    /** @copydoc EditorLocalAgentHistory::RecoveryStartPending */
    bool EditorLocalAgentHistory::RecoveryStartPending() const noexcept {
        return recoveryPrompt_.has_value() || (pending_ && pending_->action == Action::StartRecovery);
    }

    /** @copydoc EditorLocalAgentHistory::HasRecovery */
    bool EditorLocalAgentHistory::HasRecovery() const noexcept {
        return recovery_.has_value();
    }

    /** @copydoc EditorLocalAgentHistory::ReviewRecoveryAnswer */
    void EditorLocalAgentHistory::ReviewRecoveryAnswer(std::string_view answer) {
        if (!recovery_)
            return;
        auto prepared = AgentHistoryAnswerReview::Prepare(recovery_->binding, answer);
        if (prepared.HasError()) {
            FinishRecovery(AgentHistoryMessageState::Incomplete);
            return;
        }
        recovery_->answer.emplace(std::move(prepared).Value());
    }

    /** @copydoc EditorLocalAgentHistory::FinishRecovery */
    void EditorLocalAgentHistory::FinishRecovery(const AgentHistoryMessageState state) {
        if (!recovery_ || pending_)
            return;
        const auto &binding = recovery_->binding;
        auto submitted = history_.Checkpoint(binding.project, {.conversation = binding.turn.conversation,
                                                               .response = binding.turn.response,
                                                               .state = state,
                                                               .now = std::max(recovery_->checkpointTime, WallSeconds())});
        if (Queue(std::move(submitted), Action::FinishRecovery))
            recovery_->answer.reset();
        else if (!recovery_->answer)
            recovery_.reset();
    }

    bool EditorLocalAgentHistory::Button(const ILocalizationService &locale, const char *key, bool enabled) const {
        const auto label = LocalizedText(locale, key) + "##" + key;
        return Ui::Button({.label = label.c_str(), .enabled = enabled});
    }

    bool EditorLocalAgentHistory::Queue(const Result<AgentHistoryOperationId> &operation, Action action,
                                        const std::uint64_t checkpointTime) {
        if (operation.HasError()) {
            status_ = "history_write_error";
            return false;
        }
        pending_ = Pending{operation.Value(), action, checkpointTime};
        export_.reset();
        status_ = action == Action::Save ? "history_saving" : "history_working";
        return true;
    }

    void EditorLocalAgentHistory::DrawRecovery(const ILocalizationService &locale, const AgentHistorySessionView &view) {
        const bool enabled = Available(view) && !pending_;
        if (recoveryPrompt_) {
            ImGui::TextWrapped("%s", LocalizedText(locale, "history_recovery_prompt").c_str());
            DrawText(recoveryPrompt_->Prompt());
            const auto now = WallSeconds();
            if (Button(locale, "history_recovery_start", enabled) &&
                Queue(recoveryPrompt_->StartApproved(history_, LocalizedText(locale, "history_title"), now), Action::StartRecovery, now))
                recoveryPrompt_.reset();
            ImGui::SameLine();
            if (Button(locale, "history_cancel", !pending_))
                recoveryPrompt_.reset();
        }
        if (!recovery_ || !recovery_->answer)
            return;
        ImGui::TextWrapped("%s", LocalizedText(locale, "history_recovery_answer").c_str());
        DrawText(recovery_->answer->Answer());
        if (Button(locale, "history_save", enabled))
            static_cast<void>(Queue(recovery_->answer->SaveApproved(history_, std::max(recovery_->checkpointTime, WallSeconds())),
                                    Action::FinishRecovery));
        ImGui::SameLine();
        if (Button(locale, "history_recovery_keep_incomplete", enabled && !pending_))
            FinishRecovery(AgentHistoryMessageState::Incomplete);
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
        for (const auto &message : messages.last(std::min(messages.size(), std::size_t{8}))) {
            if (const auto key = ResponseStateKey(message.state))
                ImGui::TextUnformatted(LocalizedText(locale, key).c_str());
            DrawText(message.text);
        }
    }

    void EditorLocalAgentHistory::DrawActions(const EditorGuiContext &context, const AgentHistorySessionView &view,
                                              const AgentConversation &conversation) {
        const auto &locale = context.localization;
        const bool enabled = Available(view) && !pending_ && !recovery_ && !recoveryPrompt_;
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
        if (const auto popup = LocalizedText(locale, "history_delete") + "###LocalHistoryDelete";
            !ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;
        if (const auto found = deletion_ ? std::ranges::find(view.conversations, deletion_->conversation, &AgentConversation::id)
                                         : view.conversations.end();
            found == view.conversations.end() || deletion_->project != view.project) {
            deletion_.reset();
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped("%s", LocalizedText(locale, "history_delete_confirm").c_str());
            DrawText(found->title);
            const bool cancelled = Button(locale, "history_cancel");
            ImGui::SameLine();
            const bool deleted = Button(locale, "history_delete", Available(view) && !pending_ && !recovery_ && !recoveryPrompt_) &&
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
        DrawRecovery(context.localization, view);
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
