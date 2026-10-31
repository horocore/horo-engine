#include "editor/modals/prefab_creation/PrefabCreationModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Paths.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @copydoc PrefabCreationModal::PrefabCreationModal */
    PrefabCreationModal::PrefabCreationModal(const EditorGuiContext &context, PrefabCreationIntent intent, std::filesystem::path root,
                                             Submit submit)
        : EditorModal(kTextScope), context_(context), intent_(std::move(intent)), root_(std::move(root)), submit_(std::move(submit)) {
        const auto path = ProjectLayout::AssetRoot(root_).filename() / "Prefab.prefab";
        const auto text = path.generic_u8string();
        relative_.assign(text.begin(), text.end());
    }

    /** @copydoc PrefabCreationModal::Id */
    ModalId PrefabCreationModal::Id() const {
        return ModalId{0x5052454641424352ULL};
    }

    /** @copydoc PrefabCreationModal::Presentation */
    ModalPresentation PrefabCreationModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact, .dimWorkspace = true};
    }

    /** @copydoc PrefabCreationModal::ClosePolicy */
    ModalClosePolicy PrefabCreationModal::ClosePolicy() const {
        return {.allowCloseButton = true, .allowEscape = true, .allowOutsideClick = false, .allowApplicationShutdown = true};
    }

    /** @copydoc PrefabCreationModal::OnOpen */
    Result<void> PrefabCreationModal::OnOpen(EditorModalContext &) {
        return Result<void>::Success();
    }

    /** @copydoc PrefabCreationModal::CanClose */
    CloseDecision PrefabCreationModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @copydoc PrefabCreationModal::Draw */
    ModalFrameResult PrefabCreationModal::Draw() {
        const auto label = [this](const char *key) {
            return PrefabCreationModal::kTextScope.Text(context_.localization, key);
        };
        const auto title = label("create.title");
        Ui::ScopedModalShell modal({.id = "CreatePrefabFromSelection",
                                    .title = title.c_str(),
                                    .requestedSize = {600, 420},
                                    .viewportPadding = 40,
                                    .minimumWidth = 360,
                                    .minimumHeight = 300,
                                    .footerHeight = Theme::Layout::FooterH,
                                    .showClose = true,
                                    .titleFontSize = Theme::TextPx::Title()},
                                   context_.theme.fonts);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::BeginChild("##CreatePrefabBody", {0, modal.BodyHeight()});
        ImGui::TextWrapped("%s", label("create.help").c_str());
        Ui::FieldLabel(label("create.path").c_str(), context_.theme.fonts);
        (void)Ui::InputTextControl("##PrefabDestination", relative_, 4096, context_.theme.fonts,
                                   Ui::InputTextOptions{.error = error_.has_value()});
        if (error_)
            Ui::ErrorText(error_->c_str(), context_.theme.fonts);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        modal.BeginFooter({20, 12});
        const bool cancel = Ui::Button(
            {.label = label("create.cancel").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context_.theme.fonts.sans});
        ImGui::SameLine();
        const bool confirm =
            Ui::Button({.label = label("create.confirm").c_str(), .enabled = !relative_.empty(), .font = context_.theme.fonts.sans});
        modal.EndFooter();
        if (cancel || modal.CloseRequested())
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        if (confirm && !relative_.empty() && submit_) {
            const std::u8string path{relative_.begin(), relative_.end()};
            intent_.destination = root_ / std::filesystem::path{path};
            error_ = submit_(intent_);
            if (!error_)
                return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
        }
        return ModalFrameResult::None();
    }
}  // namespace Horo::Editor
