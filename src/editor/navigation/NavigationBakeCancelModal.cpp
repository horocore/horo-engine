#include "NavigationBakeCancelModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

namespace Horo::Editor {
    /** @copydoc NavigationBakeCancelModal::NavigationBakeCancelModal */
    NavigationBakeCancelModal::NavigationBakeCancelModal(const EditorGuiContext &context, NavigationBakeCancelDecision decision)
        : context_(context), decision_(std::move(decision)) {}

    ModalId NavigationBakeCancelModal::Id() const {
        return ModalId{0x4e415643414e434cULL};
    }

    ModalPresentation NavigationBakeCancelModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact};
    }

    ModalClosePolicy NavigationBakeCancelModal::ClosePolicy() const {
        return {};
    }

    Result<void> NavigationBakeCancelModal::OnOpen(EditorModalContext &) {
        return Result<void>::Success();
    }

    CloseDecision NavigationBakeCancelModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    ModalFrameResult NavigationBakeCancelModal::Draw() {
        const auto text = [this](const char *key) -> const std::string & {
            return context_.localization.Get("editor", std::string{"workspace.navigation_bake."} + key);
        };
        const auto &fonts = context_.theme.fonts;
        Ui::ScopedModalShell shell({.id = "NavigationBakeCancel",
                                    .title = text("cancel_title").c_str(),
                                    .requestedSize = {480, 400},
                                    .minimumWidth = 240,
                                    .minimumHeight = 280},
                                   fonts);
        ImGui::BeginChild("##NavigationCancelBody", {0, shell.BodyHeight()}, false,
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        Ui::Hint(text("cancel_hint").c_str(), fonts);
        if (error_) {
            Ui::ErrorText(text("action_failed").c_str(), fonts);
            Ui::Hint(error_->code.Value().c_str(), fonts);
        }
        const bool keep = Ui::Button({.label = text("keep_running").c_str(),
                                      .variant = Ui::ButtonVariant::Secondary,
                                      .font = fonts.sans,
                                      .style = {.width = Ui::StyleWidth::FillAvailable}});
        const bool back = Ui::Button({.label = text("return").c_str(),
                                      .variant = Ui::ButtonVariant::Secondary,
                                      .font = fonts.sans,
                                      .style = {.width = Ui::StyleWidth::FillAvailable}});
        ImGui::EndChild();
        const float padding = DesignSystem::SpacingFor(Theme::GetActiveTokens(), DesignSystem::SpacingSize::Small);
        shell.BeginFooter({padding, padding});
        const bool confirm =
            Ui::Button({.label = text("confirm_cancel").c_str(), .font = fonts.sans, .style = {.width = Ui::StyleWidth::FillAvailable}});
        shell.EndFooter();
        if (confirm) {
            const auto result = decision_.Confirm();
            if (result.HasValue())
                return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
            error_ = result.ErrorValue();
        }
        return keep || back || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled)
                                                      : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
