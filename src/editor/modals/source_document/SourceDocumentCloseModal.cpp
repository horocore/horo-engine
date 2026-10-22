#include "Horo/Editor/GuiLiveReload.h"
#include "SourceDocumentCloseModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    /** @copydoc SourceDocumentCloseModal::SourceDocumentCloseModal */
    SourceDocumentCloseModal::SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller,
                                                       std::weak_ptr<const void> lifetime, SourceSaveRequest request, std::string source)
        : context_(context), controller_(&controller), lifetime_(std::move(lifetime)), request_(std::move(request)),
          source_(std::move(source)) {}

    /** @copydoc SourceDocumentCloseModal::Id */
    ModalId SourceDocumentCloseModal::Id() const {
        return ModalId{0x534F55524345434CULL};
    }

    /** @copydoc SourceDocumentCloseModal::Presentation */
    ModalPresentation SourceDocumentCloseModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact};
    }

    /** @copydoc SourceDocumentCloseModal::ClosePolicy */
    ModalClosePolicy SourceDocumentCloseModal::ClosePolicy() const {
        return {};
    }

    /** @copydoc SourceDocumentCloseModal::OnOpen */
    Result<void> SourceDocumentCloseModal::OnOpen(EditorModalContext &) {
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentCloseModal::CanClose */
    CloseDecision SourceDocumentCloseModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @copydoc SourceDocumentCloseModal::Resolve */
    Result<bool> SourceDocumentCloseModal::Resolve(const SourceCloseDecision decision) {
        if (decision == SourceCloseDecision::Cancel)
            return Result<bool>::Success(true);
        if (lifetime_.expired()) {
            failed_ = true;
            return Result<bool>::Failure(MakeError(GuiLive::HostSymbol<&SourceDocumentErrors::Stale>()));
        }
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CloseSourceDocument;
        command.sourceSave = request_;
        command.sourceCloseDecision = decision;
        controller_->ProcessCommand(command);
        const auto &outcome = controller_->SourceCloseOutcome();
        failed_ = !outcome || outcome->HasError() || !outcome->Value();
        return outcome ? *outcome : Result<bool>::Failure(MakeError(GuiLive::HostSymbol<&SourceDocumentErrors::Invalid>()));
    }

    /** @copydoc SourceDocumentCloseModal::Text */
    const std::string &SourceDocumentCloseModal::Text(const char *suffix) const {
        return context_.localization.Get("editor", std::string{"workspace.documents."} + suffix);
    }

    /** @copydoc SourceDocumentCloseModal::LogicalFooterHeight */
    float SourceDocumentCloseModal::LogicalFooterHeight(const DesignSystem::DesignTokens &tokens) noexcept {
        const float physical =
            DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight * 3.0F + tokens.spacing.propertyRowGap * 2.0F;
        return physical / tokens.sizes.uiScale + 24.0F;
    }

    /** @copydoc SourceDocumentCloseModal::Draw */
    ModalFrameResult SourceDocumentCloseModal::Draw() {
        const auto &fonts = context_.theme.fonts;
        const auto &tokens = Theme::GetActiveTokens();
        Ui::ScopedModalShell shell({.id = "SourceDocumentClose",
                                    .title = Text("close_title").c_str(),
                                    .requestedSize = {480, 400},
                                    .minimumWidth = 240,
                                    .minimumHeight = 280,
                                    .footerHeight = LogicalFooterHeight(tokens)},
                                   fonts);
        ImGui::BeginChild("##SourceCloseBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text("close_prompt"), Theme::Text(), fonts);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, source_, Theme::Muted(), fonts);
        if (failed_)
            Ui::ErrorText(Text("close_failed").c_str(), fonts);
        ImGui::EndChild();
        shell.BeginFooter({16, 12});
        const auto button = [&](const char *key, const SourceCloseDecision decision) {
            if (!Ui::Button({.label = Text(key).c_str(),
                             .variant = Ui::ButtonVariant::Secondary,
                             .font = fonts.sans,
                             .style = {.width = Ui::StyleWidth::FillAvailable}}))
                return false;
            const auto resolved = Resolve(decision);
            return resolved.HasValue() && resolved.Value();
        };
        const bool close = button("save_close", SourceCloseDecision::Save) || button("discard_close", SourceCloseDecision::Discard) ||
                           button("cancel", SourceCloseDecision::Cancel);
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Completed) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
