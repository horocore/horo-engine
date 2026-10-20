#include "editor/modals/world_layer/CreateWorldLayerModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Utf8.h"

#include <utility>

namespace Horo::Editor {
    /** @copydoc CreateWorldLayerModal::CreateWorldLayerModal */
    CreateWorldLayerModal::CreateWorldLayerModal(const EditorGuiContext &context, const WorldDocumentFence expected, Create create)
        : context_(context), expected_(expected), create_(std::move(create)) {}

    /** @copydoc CreateWorldLayerModal::Id */
    ModalId CreateWorldLayerModal::Id() const {
        return ModalId{0x574F524C444C4159ULL};
    }

    /** @copydoc CreateWorldLayerModal::Presentation */
    ModalPresentation CreateWorldLayerModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact};
    }

    /** @copydoc CreateWorldLayerModal::ClosePolicy */
    ModalClosePolicy CreateWorldLayerModal::ClosePolicy() const {
        return {};
    }

    /** @copydoc CreateWorldLayerModal::OnOpen */
    Result<void> CreateWorldLayerModal::OnOpen(EditorModalContext &) {
        return create_ ? Result<void>::Success() : Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
    }

    /** @copydoc CreateWorldLayerModal::CanClose */
    CloseDecision CreateWorldLayerModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @copydoc CreateWorldLayerModal::DrawName */
    bool CreateWorldLayerModal::DrawName() {
        const auto &fonts = context_.theme.fonts;
        const auto label = context_.localization.Get("editor", "workspace.world_layers.name");
        Ui::FieldLabel(label.c_str(), fonts);
        if (Ui::InputTextControl("##LayerName", name_, 1025, fonts))
            failureKey_.clear();
        const bool valid = !name_.empty() && name_.find('\0') == std::string::npos && IsValidUtf8ScalarSequence(name_);
        if (!failureKey_.empty())
            Ui::ErrorText(context_.localization.Get("editor", failureKey_).c_str(), fonts);
        else if (!valid)
            Ui::Hint(context_.localization.Get("editor", "workspace.world_layers.name_hint").c_str(), fonts);
        return valid;
    }

    /** @copydoc CreateWorldLayerModal::Draw */
    ModalFrameResult CreateWorldLayerModal::Draw() {
        const auto title = context_.localization.Get("editor", "workspace.world_layers.create");
        const auto &fonts = context_.theme.fonts;
        const float footerHeight = 2.0F * DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium).minimumHeight +
                                   ImGui::GetStyle().ItemSpacing.y + 2.0F * Theme::Layout::CardPad;
        Ui::ScopedModalShell shell({.id = "CreateWorldLayer",
                                    .title = title.c_str(),
                                    .requestedSize = {520.0F, 320.0F},
                                    .minimumWidth = 280.0F,
                                    .minimumHeight = 240.0F,
                                    .footerHeight = footerHeight},
                                   fonts);
        ImGui::BeginChild("##WorldLayerDraft", {0.0F, shell.BodyHeight()});
        const bool valid = DrawName();
        ImGui::EndChild();
        shell.BeginFooter({Theme::Layout::CardPad, Theme::Layout::CardPad});
        const auto cancel = context_.localization.Get("editor", "workspace.world_layers.cancel");
        if (Ui::Button({.label = cancel.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans})) {
            shell.EndFooter();
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        }
        const bool submit = Ui::Button({.label = title.c_str(), .enabled = valid, .font = fonts.sans});
        shell.EndFooter();
        if (shell.CloseRequested())
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        if (submit) {
            const auto result = create_(name_, expected_);
            if (result.HasValue())
                return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
            const auto code = result.ErrorValue().code;
            failureKey_ = code == WorldDocumentErrors::Stale.code || code == WorldDocumentErrors::Lifecycle.code
                              ? "workspace.world_layers.stale"
                              : "workspace.world_layers.rejected";
        }
        return ModalFrameResult::None();
    }
}  // namespace Horo::Editor
