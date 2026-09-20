#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Network/NetworkProjectSettings.h"
#include "SettingsModalInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <imgui.h>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace Horo::Editor::SettingsModalInternal {
    using namespace Theme;
    using namespace Ui;
    using Theme::ScopedTextStyle;

    void DrawFooterStatus(const SettingsState &st, const EditorGuiContext &ctx, const float footerPaddingX) {
        ImGui::SetCursorPos({footerPaddingX, (ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5F});
        if (st.dirty) {
            ScopedTextStyle badge(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Warn());
            ImGui::TextUnformatted("unsaved");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0F, 8.0F);
        }
        ScopedTextStyle hint(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
        const bool hasFeedback = !st.modalFeedback.empty();
        ImVec4 textColor = Dim();
        if (hasFeedback)
            textColor = Accent();
        else if (st.statusIsError)
            textColor = Err();
        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        const char *message = "Apply writes user preferences to ~/.horo/editor_settings.json";
        if (hasFeedback)
            message = st.modalFeedback.c_str();
        else if (!st.statusMessage.empty())
            message = st.statusMessage.c_str();
        ImGui::TextUnformatted(message);
        ImGui::PopStyleColor();
    }

    [[nodiscard]] bool DrawFooterActions(SettingsState &st, EditorSettingsService &settings, const EditorGuiContext &ctx,
                                         const float centeredActionY) {
        constexpr float actionH = 32.0F;
        constexpr float restoreW = 124.0F;
        constexpr float cancelW = 78.0F;
        constexpr float applyW = 70.0F;
        constexpr float gap = 8.0F;
        constexpr float footerPaddingX = 22.0F;
        const float actionsW = restoreW + cancelW + applyW + gap * 2.0F;
        ImGui::SetCursorPos({ImGui::GetWindowWidth() - footerPaddingX - actionsW, centeredActionY});
        bool requestClose = false;
        if (const std::string restoreDefaults = ctx.localization.Get("editor", "settings.restore_defaults");
            Button({.label = restoreDefaults.c_str(),
                    .size = {restoreW, actionH},
                    .variant = ButtonVariant::Secondary,
                    .font = ctx.theme.fonts.sansCompact,
                    .baseFontSize = FontPx::SansCompact,
                    .componentSize = ComponentSize::Small})) {
            LOG_INFO("editor.settings", "Restore Defaults clicked — draft reset to factory defaults.");
            ApplySettingsToDraft(st, DefaultEditorSettings());
            st.statusMessage = "Defaults loaded into draft. Apply to persist.";
            st.statusIsError = false;
        }
        ImGui::SameLine(0.0F, gap);
        if (const std::string cancelLabel = ctx.localization.Get("editor", "settings.cancel") + "###settings_cancel";
            Button({.label = cancelLabel.c_str(),
                    .size = {cancelW, actionH},
                    .variant = ButtonVariant::Secondary,
                    .font = ctx.theme.fonts.sansCompact,
                    .baseFontSize = FontPx::SansCompact,
                    .componentSize = ComponentSize::Small})) {
            LOG_INFO("editor.settings", "Settings cancelled by user (dirty=%s).", st.dirty ? "yes" : "no");
            requestClose = true;
        }
        ImGui::SameLine(0.0F, gap);
        if (const std::string applyLabel = ctx.localization.Get("editor", "settings.apply") + "###settings_apply";
            Button({.label = applyLabel.c_str(),
                    .size = {applyW, actionH},
                    .variant = ButtonVariant::Primary,
                    .font = ctx.theme.fonts.sansCompact,
                    .baseFontSize = FontPx::SansCompact,
                    .componentSize = ComponentSize::Small})) {
            (void)ApplySettings(st, settings);
        }
        return requestClose;
    }

    [[nodiscard]] bool DrawFooterContent(SettingsState &st, EditorSettingsService &settings, const EditorGuiContext &ctx) {
        constexpr float actionH = 32.0F;
        constexpr float footerPaddingX = 22.0F;
        const float centeredActionY = (ImGui::GetWindowHeight() - actionH) * 0.5F;
        DrawFooterStatus(st, ctx, footerPaddingX);
        return DrawFooterActions(st, settings, ctx, centeredActionY);
    }

}  // namespace Horo::Editor::SettingsModalInternal
