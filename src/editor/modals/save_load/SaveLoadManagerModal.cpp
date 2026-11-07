#include "SaveLoadManagerModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <array>

namespace Horo::Editor {
    /** @copydoc SaveLoadManagerModal::SaveLoadManagerModal */
    SaveLoadManagerModal::SaveLoadManagerModal(const EditorGuiContext &context, std::weak_ptr<EditorPlayRuntimeBinding> binding)
        : EditorModal(kTextScope), context_(context), binding_(std::move(binding)) {}

    /** @copydoc EditorModal::Id */
    ModalId SaveLoadManagerModal::Id() const {
        return ModalId{0x504c415953415645ULL};
    }

    /** @copydoc EditorModal::Presentation */
    ModalPresentation SaveLoadManagerModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    /** @copydoc EditorModal::ClosePolicy */
    ModalClosePolicy SaveLoadManagerModal::ClosePolicy() const {
        return {};
    }

    /** @copydoc EditorModal::CanClose */
    CloseDecision SaveLoadManagerModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @copydoc EditorModal::OnOpen */
    Result<void> SaveLoadManagerModal::OnOpen(EditorModalContext &) {
        OnUpdate(0);
        return Result<void>::Success();
    }

    /** @brief Resolves only a compiled closed localization vocabulary in the existing Editor catalog. */
    const std::string &SaveLoadManagerModal::Text(const char *key) const {
        return LocalizedText(context_.localization, key);
    }

    /** @brief Copies a bounded catalog page only when the actual owner publishes a changed immutable projection. */
    void SaveLoadManagerModal::OnUpdate(float) {
        const auto binding = binding_.lock();
        available_ = binding && !binding->revoked && binding->save;
        if (!available_)
            return;
        auto current = binding->save->Snapshot();
        if (!current || current == snapshot_)
            return;
        snapshot_ = std::move(current);
        if (!snapshot_->manager)
            return;
        const auto page = snapshot_->manager->Page({.maximumRows = 16});
        if (page.HasError()) {
            error_ = page.ErrorValue();
            return;
        }
        page_ = page.Value();
    }

    namespace {
        /** @brief Withdraws an unadmitted UI intent; an already queued cancellation remains idempotently queued. */
        bool WithdrawQueuedIntent(EditorPlayRuntimeBinding &binding, PlaySaveCommandKind kind) {
            if (kind != PlaySaveCommandKind::Cancel || !binding.saveCommand)
                return false;
            if (binding.saveCommand->kind != PlaySaveCommandKind::Cancel)
                binding.saveCommand.reset();
            return true;
        }
    }  // namespace

    /** @copydoc SaveLoadManagerModal::Send */
    void SaveLoadManagerModal::Send(PlaySaveCommandKind kind) {
        const auto binding = binding_.lock();
        if (!binding || binding->revoked || !binding->save)
            return;
        if (WithdrawQueuedIntent(*binding, kind) || binding->saveCommand)
            return;
        PlaySaveCommand command{.kind = kind, .displayName = displayName_};
        const auto consent = Consent(kind);
        if (consent.HasError()) {
            error_ = consent.ErrorValue();
            return;
        }
        command.selection = consent.Value();
        error_.reset();
        binding->saveCommand.emplace(std::move(command));
    }

    /** @copydoc SaveLoadManagerModal::Consent */
    Result<std::optional<Runtime::SaveManagerCommand>> SaveLoadManagerModal::Consent(PlaySaveCommandKind kind) const {
        using Outcome = Result<std::optional<Runtime::SaveManagerCommand>>;
        if (kind != PlaySaveCommandKind::Load && kind != PlaySaveCommandKind::Delete)
            return Outcome::Success({});
        if (!snapshot_ || !snapshot_->manager || !selected_)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        const auto consent = snapshot_->manager->Command(kind == PlaySaveCommandKind::Load ? Runtime::SaveManagerCommandKind::Load
                                                                                           : Runtime::SaveManagerCommandKind::Delete,
                                                         *selected_);
        return consent.HasError() ? Outcome::Failure(consent.ErrorValue()) : Outcome::Success(consent.Value());
    }

    /** @copydoc SaveLoadManagerModal::Action */
    void SaveLoadManagerModal::Action(const char *key, PlaySaveCommandKind kind, bool enabled) {
        if (Ui::Button({.label = Text(key).c_str(), .enabled = enabled, .font = context_.theme.fonts.sans}))
            Send(kind);
    }

    /** @copydoc SaveLoadManagerModal::DrawSlots */
    void SaveLoadManagerModal::DrawSlots() {
        if (page_.rows.empty()) {
            Ui::Hint(Text("empty").c_str(), context_.theme.fonts);
            return;
        }
        for (std::size_t index = 0; index < page_.rows.size(); ++index) {
            const auto &row = page_.rows[index];
            ImGui::PushID(static_cast<int>(index));
            const auto &label = row.displayName.empty() ? Text("unnamed") : row.displayName;
            if (ImGui::Selectable(label.c_str(), selected_ && *selected_ == row.slot))
                selected_ = row.slot;
            ImGui::PopID();
        }
    }

    /** @copydoc SaveLoadManagerModal::DrawActions */
    void SaveLoadManagerModal::DrawActions() {
        const auto binding = binding_.lock();
        const bool pending = !binding || binding->saveCommand || (snapshot_ && snapshot_->operationPending);
        const bool ready = available_ && snapshot_ && snapshot_->availability == PlaySaveAvailability::Ready;
        Ui::FieldLabel(Text("name").c_str(), context_.theme.fonts);
        (void)Ui::InputTextControl("##PlaySaveName", displayName_, 128, context_.theme.fonts);
        const bool admitted = ready && !pending;
        const bool selection = admitted && selected_.has_value();
        Action("save", PlaySaveCommandKind::SaveNew, admitted);
        Action("load", PlaySaveCommandKind::Load, selection);
        Action("delete", PlaySaveCommandKind::Delete, selection);
        Action("refresh", PlaySaveCommandKind::Refresh, available_ && !pending);
        Action("cancel", PlaySaveCommandKind::Cancel, available_ && pending);
    }

    /** @copydoc SaveLoadManagerModal::DrawStatus */
    void SaveLoadManagerModal::DrawStatus() {
        Ui::Hint(Text("lifetime").c_str(), context_.theme.fonts);
        if (!available_)
            Ui::Hint(Text("unavailable").c_str(), context_.theme.fonts);
        const auto binding = binding_.lock();
        const Error *failure = error_ ? &*error_ : nullptr;
        if (snapshot_ && snapshot_->error)
            failure = &*snapshot_->error;
        if (binding && binding->saveError)
            failure = &*binding->saveError;
        if (failure)
            Ui::ErrorText(failure->code.Value().c_str(), context_.theme.fonts);
        if (snapshot_ && snapshot_->operationPending)
            Ui::Hint(Text("pending").c_str(), context_.theme.fonts);
    }

    namespace {
        /** @brief Uses a closed localized vocabulary for actual canonical operation state. */
        const char *OperationStateKey(Runtime::SaveOperationState state) noexcept {
            using enum Runtime::SaveOperationState;
            switch (state) {
                case Queued:
                    return "state_queued";
                case Running:
                    return "state_running";
                case Completed:
                    return "state_completed";
                case Failed:
                    return "state_failed";
                case Cancelled:
                    return "state_cancelled";
            }
            return "state_running";
        }

        /** @brief Preserves the canonical stage order without deriving progress from elapsed time or frame count. */
        const char *OperationStageKey(Runtime::SaveOperationStage stage) noexcept {
            constexpr std::array keys{"stage_queued", "stage_capture", "stage_encode",  "stage_finalize", "stage_write",
                                      "stage_commit", "stage_verify",  "stage_migrate", "stage_prepare",  "stage_ready",
                                      "stage_apply",  "stage_refresh", "stage_delete"};
            const auto index = static_cast<std::size_t>(stage);
            return index < keys.size() ? keys[index] : "state_running";
        }
    }  // namespace

    /** @copydoc SaveLoadManagerModal::DrawOperation */
    void SaveLoadManagerModal::DrawOperation() {
        if (!snapshot_ || !snapshot_->manager)
            return;
        for (const auto &operation : snapshot_->manager->Operations()) {
            Ui::Hint(Text(OperationStateKey(operation.state)).c_str(), context_.theme.fonts);
            Ui::Hint(Text(OperationStageKey(operation.stage)).c_str(), context_.theme.fonts);
            if (operation.state == Runtime::SaveOperationState::Running)
                ImGui::ProgressBar(static_cast<float>(operation.progress.Fraction()), {-1, 0});
            if (operation.commit == Runtime::SaveOperationCommitOutcome::Committed)
                Ui::Hint(Text("committed").c_str(), context_.theme.fonts);
            if (operation.state == Runtime::SaveOperationState::Failed && operation.commit == Runtime::SaveOperationCommitOutcome::Unknown)
                Ui::ErrorText(Text("unknown_commit").c_str(), context_.theme.fonts);
        }
    }

    /** @brief Renders existing shared modal controls; every action remains a typed intent rather than storage access. */
    ModalFrameResult SaveLoadManagerModal::Draw() {
        Ui::ScopedModalShell shell({.id = "PlaySaveManager",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {680, 540},
                                    .minimumWidth = 280,
                                    .minimumHeight = 260,
                                    .headerHeight = 38,
                                    .footerHeight = 56,
                                    .titleFontSize = Theme::TextPx::Label()},
                                   context_.theme.fonts);
        ImGui::BeginChild("##PlaySaveBody", {0, shell.BodyHeight()});
        ImGui::PushTextWrapPos(0);
        DrawStatus();
        DrawOperation();
        ImGui::PopTextWrapPos();
        DrawSlots();
        DrawActions();
        ImGui::EndChild();
        shell.BeginFooter({16, 12});
        const bool close =
            Ui::Button({.label = Text("close").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context_.theme.fonts.sans});
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
