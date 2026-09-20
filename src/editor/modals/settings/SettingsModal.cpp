#include "editor/modals/settings/SettingsModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "SettingsModalInternal.h"

#include <imgui.h>

namespace Horo::Editor {
    namespace {
        using namespace Theme;
        using namespace Ui;
        using Theme::ScopedTextStyle;
        using namespace SettingsModalInternal;

        [[nodiscard]] ModalFrameResult DrawSettingsModalPresentationImpl(SettingsState &st, EditorSettingsService &settings,
                                                                         const EditorGuiContext &ctx, const ImTextureID logo) {
            if (st.appearance.pendingThemeIndex >= 0) {
                SelectThemeByIndex(st.appearance.pendingThemeIndex);
                st.appearance.pendingThemeIndex = -1;
            }

            st.modalFeedback.clear();

            const std::string title = ctx.localization.Get("editor", "settings.title");
            ScopedModalShell modal(
                {
                    .id = "Settings",
                    .title = title.c_str(),
                    .requestedSize = {SettingsModalInternal::Layout::ModalW, SettingsModalInternal::Layout::ModalH},
                    .viewportPadding = SettingsModalInternal::Layout::ViewportPad,
                    .headerHeight = SettingsModalInternal::Layout::HeaderH,
                    .footerHeight = SettingsModalInternal::Layout::FooterH,
                    .logo = logo,
                    .showBrandMark = true,
                    .titleFontSize = TextPx::Title(),
                },
                ctx.theme.fonts);
            ModalSplitPane(
                {
                    .id = "SettingsBody",
                    .size = {0.0F, modal.BodyHeight()},
                    .leadingWidth = SettingsModalInternal::Layout::NavW,
                    .leadingPadding = {8.0F, 10.0F},
                    .contentPadding = {26.0F, 22.0F},
                    .leadingBackground = Bg0(),
                    .contentBackground = Bg1(),
                    .drawDivider = true,
                    .leadingScrollable = false,
                    .contentScrollable = true,
                },
                [&st, &ctx]() {
                DrawNavigationContent(st, ctx);
            }, [&st, &ctx]() {
                DrawContent(st, ctx);
            });
            st.dirty = CollectDraftSettings(st) != st.committed;
            modal.BeginFooter({0.0F, 0.0F});
            const bool footerRequestedClose = DrawFooterContent(st, settings, ctx);
            modal.EndFooter();

            return (modal.CloseRequested() || footerRequestedClose) ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled)
                                                                    : ModalFrameResult::None();
        }

    }  // namespace

    ModalFrameResult DrawSettingsModalPresentation(SettingsState &state, EditorSettingsService &settings, const EditorGuiContext &ctx,
                                                   const ImTextureID logo) {
        return DrawSettingsModalPresentationImpl(state, settings, ctx, logo);
    }

    ModalFrameResult SettingsModal::Draw() {
        return DrawSettingsModalPresentation(m_draft, m_settings, m_context, m_logo);
    }

}  // namespace Horo::Editor
