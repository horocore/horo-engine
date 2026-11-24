#include "EditorLocalAgentHistory.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <chrono>
#include <utility>

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

        /** @brief Resolves a stored reference kind to a localized display label. */
        const char *ReferenceKey(const AgentHistoryReferenceKind kind) {
            using enum AgentHistoryReferenceKind;
            switch (kind) {
                case Asset:
                    return "history_reference_asset";
                case Document:
                    return "history_reference_document";
                case ToolCall:
                    return "history_reference_tool";
                case BuildOutput:
                    return "history_reference_build";
            }
            return "history_reference_record";
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
        referenceReview_.reset();
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
        if (action == Action::SaveDraft) {
            AdoptDraftReceipt(!completed->error, accepted.checkpointTime);
            return false;
        }
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

    /** @copydoc EditorLocalAgentHistory::AdoptDraftReceipt */
    void EditorLocalAgentHistory::AdoptDraftReceipt(const bool saved, const std::uint64_t checkpointTime) {
        status_ = saved ? "history_draft_saved" : "history_write_error";
        if (!recovery_)
            return;
        if (saved)
            recovery_->checkpointTime = checkpointTime;
        if (const auto settlement = std::exchange(recovery_->pendingSettlement, std::nullopt))
            FinishRecovery(*settlement);
    }

    /** @copydoc EditorLocalAgentHistory::Project */
    AgentHistoryProjectRevision EditorLocalAgentHistory::Project() const noexcept {
        return history_.View().project;
    }

    /** @copydoc EditorLocalAgentHistory::Review */
    void EditorLocalAgentHistory::Review(AgentHistoryProjectRevision project, std::string_view prompt, std::string_view answer,
                                         const AgentHistoryUsage usage) {
        if (project != Project())
            return;
        auto prepared = AgentHistoryExchangeReview::Prepare(project, prompt, answer, usage);
        if (prepared.HasValue())
            review_.emplace(std::move(prepared).Value());
    }

    /** @copydoc EditorLocalAgentHistory::ClearReview */
    void EditorLocalAgentHistory::ClearReview() noexcept {
        review_.reset();
        referenceReview_.reset();
    }

    /** @copydoc EditorLocalAgentHistory::ReviewReference */
    void EditorLocalAgentHistory::ReviewReference(const AgentHistoryProjectRevision project, const AgentHistoryReference reference) {
        if (project == Project() && Available(history_.View()))
            referenceReview_.emplace(project, reference);
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
        ClearReview();
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

    /** @copydoc EditorLocalAgentHistory::ReviewLiveDraft */
    void EditorLocalAgentHistory::ReviewLiveDraft(const AgentHistoryLiveDraft &draft) {
        if (!recovery_ || pending_ || recovery_->answer)
            return;
        auto preview = draft.Review(recovery_->binding);
        if (preview.HasValue()) {
            recovery_->draft.emplace(std::move(preview).Value());
            status_ = nullptr;
        } else {
            status_ = "history_draft_unavailable";
        }
    }

    /** @copydoc EditorLocalAgentHistory::ReviewRecoveryAnswer */
    void EditorLocalAgentHistory::ReviewRecoveryAnswer(std::string_view answer, const AgentHistoryUsage usage) {
        if (!recovery_)
            return;
        recovery_->draft.reset();
        auto prepared = AgentHistoryAnswerReview::Prepare(recovery_->binding, answer, usage);
        if (prepared.HasError()) {
            FinishRecovery(AgentHistoryMessageState::Incomplete);
            return;
        }
        recovery_->answer.emplace(std::move(prepared).Value());
    }

    /** @copydoc EditorLocalAgentHistory::FinishRecovery */
    void EditorLocalAgentHistory::FinishRecovery(const AgentHistoryMessageState state) {
        if (!recovery_)
            return;
        recovery_->draft.reset();
        if (pending_) {
            if (pending_->action == Action::SaveDraft)
                recovery_->pendingSettlement = state;
            return;
        }
        const auto &binding = recovery_->binding;
        const auto submitted = history_.Checkpoint(binding.project, {.conversation = binding.turn.conversation,
                                                                     .response = binding.turn.response,
                                                                     .state = state,
                                                                     .now = std::max(recovery_->checkpointTime, WallSeconds())});
        if (Queue(submitted, Action::FinishRecovery))
            recovery_->answer.reset();
        else if (!recovery_->answer)
            recovery_.reset();
    }

    bool EditorLocalAgentHistory::Button(const ILocalizationService &locale, const char *key, bool enabled) {
        buttonLabel_.assign(LocalizedText(locale, key)).append("##").append(key);
        return Ui::Button({.label = buttonLabel_.c_str(), .enabled = enabled});
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
            if (Button(locale, "history_recovery_start", enabled)) {
                const auto now = WallSeconds();
                const auto started = recoveryPrompt_->StartApproved(history_, LocalizedText(locale, "history_title"), now);
                if (Queue(started, Action::StartRecovery, now))
                    recoveryPrompt_.reset();
            }
            ImGui::SameLine();
            if (Button(locale, "history_cancel", !pending_))
                recoveryPrompt_.reset();
        }
        if (!recovery_)
            return;
        DrawDraft(locale, enabled);
        if (!recovery_->answer)
            return;
        ImGui::TextWrapped("%s", LocalizedText(locale, "history_recovery_answer").c_str());
        DrawText(recovery_->answer->Answer());
        DrawTokenUsage(locale, recovery_->answer->Usage());
        if (Button(locale, "history_save", enabled))
            static_cast<void>(Queue(recovery_->answer->SaveApproved(history_, std::max(recovery_->checkpointTime, WallSeconds())),
                                    Action::FinishRecovery));
        ImGui::SameLine();
        if (Button(locale, "history_recovery_keep_incomplete", enabled && !pending_))
            FinishRecovery(AgentHistoryMessageState::Incomplete);
    }

    /** @copydoc EditorLocalAgentHistory::DrawDraft */
    void EditorLocalAgentHistory::DrawDraft(const ILocalizationService &locale, const bool enabled) {
        auto &draft = recovery_->draft;
        if (!draft)
            return;
        ImGui::TextWrapped("%s", LocalizedText(locale, "history_draft_preview").c_str());
        DrawText(draft->Answer());
        if (Button(locale, "history_draft_save", enabled)) {
            const auto now = std::max(recovery_->checkpointTime, WallSeconds());
            if (Queue(draft->SaveApproved(history_, now), Action::SaveDraft, now))
                draft.reset();
        }
        ImGui::SameLine();
        if (Button(locale, "history_draft_cancel", enabled && !pending_))
            draft.reset();
    }

    /** @copydoc EditorLocalAgentHistory::DrawTokenUsage */
    void EditorLocalAgentHistory::DrawTokenUsage(const ILocalizationService &locale, const AgentHistoryUsage usage) const {
        if (usage.inputTokens != 0)
            DrawText(LocalizedText(locale, "history_input_tokens") + " " + std::to_string(usage.inputTokens));
        if (usage.outputTokens != 0)
            DrawText(LocalizedText(locale, "history_output_tokens") + " " + std::to_string(usage.outputTokens));
    }

    void EditorLocalAgentHistory::DrawReview(const ILocalizationService &locale, const AgentHistorySessionView &view) {
        if (!review_)
            return;
        ImGui::TextUnformatted(LocalizedText(locale, "history_review").c_str());
        DrawText(review_->Prompt());
        DrawText(review_->Response());
        DrawTokenUsage(locale, review_->Usage());
        if (Button(locale, "history_save", Available(view) && !pending_) &&
            Queue(review_->SaveApproved(history_, LocalizedText(locale, "history_title"), WallSeconds()), Action::Save))
            review_.reset();
        if (view.phase == AgentHistorySessionPhase::Settled && !Available(view))
            ImGui::TextWrapped("%s", LocalizedText(locale, "history_unavailable").c_str());
    }

    /** @brief Shows the same reference in the review and saved history. */
    void EditorLocalAgentHistory::DrawReference(const ILocalizationService &locale, const AgentHistoryReference reference) const {
        DrawText(LocalizedText(locale, ReferenceKey(reference.kind)) + " #" + std::to_string(reference.identity) + " (" +
                 LocalizedText(locale, "history_reference_revision") + " " + std::to_string(reference.revision) + ")");
    }

    /** @brief Saves only the exact project-bound reference after a separate user action. */
    void EditorLocalAgentHistory::DrawReferenceReview(const ILocalizationService &locale, const AgentHistorySessionView &view) {
        if (!referenceReview_)
            return;
        ImGui::PushID("saved-reference");
        DrawText(LocalizedText(locale, "history_reference_review"));
        DrawReference(locale, referenceReview_->reference);
        if (Button(locale, "history_reference_save", Available(view) && !pending_)) {
            const auto operation =
                RecordApprovedHistoryReference(history_, referenceReview_->project, referenceReview_->reference,
                                               LocalizedText(locale, "history_title"), LocalizedText(locale, "history_reference_record"),
                                               LocalizedText(locale, "history_reference_answer"), WallSeconds());
            if (Queue(operation, Action::Save))
                referenceReview_.reset();
        }
        ImGui::SameLine();
        if (Button(locale, "history_cancel", !pending_))
            referenceReview_.reset();
        ImGui::PopID();
    }

    void EditorLocalAgentHistory::Select(const AgentConversation &conversation) {
        selected_ = conversation.id;
        title_ = conversation.title;
        export_.reset();
    }

    bool EditorLocalAgentHistory::DrawManager(const EditorGuiContext &context, const AgentHistorySessionView &view) {
        const auto &locale = context.localization;
        if (view.conversations.empty())
            return false;
        if (Button(locale, "history_manage"))
            managing_ = !managing_;
        if (!managing_)
            return false;
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
        const bool textActive = DrawActions(context, view, conversation);
        const auto messages = std::span{conversation.messages};
        if (messages.size() > 8)
            ImGui::TextUnformatted(LocalizedText(locale, "history_recent_messages").c_str());
        for (const auto &message : messages.last(std::min(messages.size(), std::size_t{8}))) {
            if (const auto key = ResponseStateKey(message.state))
                ImGui::TextUnformatted(LocalizedText(locale, key).c_str());
            DrawText(message.text);
            DrawTokenUsage(locale, message.usage);
            for (const auto reference : message.references)
                DrawReference(locale, reference);
        }
        return textActive;
    }

    bool EditorLocalAgentHistory::DrawActions(const EditorGuiContext &context, const AgentHistorySessionView &view,
                                              const AgentConversation &conversation) {
        const auto &locale = context.localization;
        const bool enabled = Available(view) && !pending_ && !recovery_ && !recoveryPrompt_;
        ImGui::TextUnformatted(LocalizedText(locale, "history_name").c_str());
        ImGui::BeginDisabled(!enabled);
        static_cast<void>(Ui::InputTextControl("##LocalHistoryName", title_, AgentHistoryLimits::MaximumTitleBytes, context.theme.fonts));
        const bool textActive = ImGui::IsItemActive();
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
        return textActive;
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
    bool EditorLocalAgentHistory::Draw(const EditorGuiContext &context) {
        const auto view = history_.View();
        DrawRecovery(context.localization, view);
        DrawReview(context.localization, view);
        DrawReferenceReview(context.localization, view);
        const bool textActive = DrawManager(context, view);
        DrawDeletion(context.localization, view);
        if (managing_ && export_) {
            DrawText(*export_);
            if (Button(context.localization, "history_copy_export"))
                ImGui::SetClipboardText(export_->c_str());
        }
        if (status_)
            ImGui::TextWrapped("%s", LocalizedText(context.localization, status_).c_str());
        return textActive;
    }
}  // namespace Horo::Editor
