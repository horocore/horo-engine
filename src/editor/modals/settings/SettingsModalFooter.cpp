#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/SettingsModal.h"
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

    namespace {
        /** @brief Selects localized feedback from typed transaction state without reading draft values. */
        const char *StatusTextKey(const SettingsState &state) {
            switch (state.status) {
                case SettingsModalStatus::Loaded:
                    return "configuration.loaded";
                case SettingsModalStatus::Applied:
                    return "configuration.applied";
                case SettingsModalStatus::Defaults:
                    return "configuration.defaults";
                case SettingsModalStatus::Failure:
                    if (state.configurationFailure && (state.configurationFailure->code.Value() == "configuration.draft_stale" ||
                                                       state.configurationFailure->code.Value() == "editor.settings.draft_stale"))
                        return "configuration.stale";
                    return "configuration.invalid";
                case SettingsModalStatus::None:
                    return static_cast<SettingsTab>(state.activeTab) == SettingsTab::ProjectConfiguration
                               ? "configuration.project_persist_hint"
                               : "configuration.persist_hint";
            }
            return "configuration.persist_hint";
        }

        /** @brief Restores one validated default preview through the shared authority without committing it. */
        void RestoreDefaultDraft(SettingsState &state, EditorSettingsService &settings) {
            LOG_INFO("editor.settings", "Restore Defaults clicked — draft reset to factory defaults.");
            if (static_cast<SettingsTab>(state.activeTab) == SettingsTab::ProjectConfiguration) {
                if (state.projectConfiguration) {
                    const auto restored = state.projectConfiguration->RestoreDefaults();
                    state.configurationFailure = restored.HasError() ? std::optional<Error>{restored.ErrorValue()} : std::nullopt;
                    state.status = restored.HasError() ? SettingsModalStatus::Failure : SettingsModalStatus::Defaults;
                }
                return;
            }
            LoadSettingsForModal(state, settings);
            const auto restored = state.configuration->RestoreDefaults();
            if (restored.HasError()) {
                state.configurationFailure = restored.ErrorValue();
                state.status = SettingsModalStatus::Failure;
                return;
            }
            auto defaults = DefaultEditorSettings();
            ApplyEditorConfigurationSnapshot(*state.configuration->Preview(), defaults);
            ApplySettingsToDraft(state, defaults);
            state.appearance.pendingThemeIndex = state.appearance.themeIndex;
            state.configurationPreviewed = true;
            state.status = SettingsModalStatus::Defaults;
        }
    }  // namespace

    void DrawFooterStatus(const SettingsState &st, const EditorGuiContext &ctx, const float footerPaddingX, const float actionsStartX) {
        ImGui::SetCursorPos({footerPaddingX, (ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5F});
        if (st.dirty || (st.configuration && !st.configuration->Draft().proposedValues.empty()) ||
            (st.projectConfiguration && !st.projectConfiguration->Draft().proposedValues.empty())) {
            ScopedTextStyle badge(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Warn());
            ImGui::TextUnformatted(SettingsModal::kTextScope.Text(ctx.localization, "configuration.unsaved").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0F, 8.0F);
        }
        ScopedTextStyle hint(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
        const bool hasFeedback = !st.modalFeedback.empty();
        ImVec4 textColor = Dim();
        if (hasFeedback)
            textColor = Accent();
        else if (st.status == SettingsModalStatus::Failure)
            textColor = Err();
        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        const auto navigationHint = SettingsModal::kTextScope.Text(ctx.localization, "navigation.save_policy_first");
        const auto &status = SettingsModal::kTextScope.Text(ctx.localization, StatusTextKey(st));
        const char *message = st.navigationProjectDraftDirty ? navigationHint.c_str() : status.c_str();
        if (hasFeedback)
            message = st.modalFeedback.c_str();
        const auto prefab = st.prefab.owner.lock();
        const bool project = static_cast<SettingsTab>(st.activeTab) == SettingsTab::ProjectConfiguration;
        const auto &prefabHint = SettingsModal::kTextScope.Text(ctx.localization, "prefab.save_first");
        if (!project && prefab && prefab->IsDirty())
            message = prefabHint.c_str();
        const auto origin = ImGui::GetWindowPos();
        ImGui::PushClipRect(origin, {origin.x + actionsStartX - 12.0F, origin.y + ImGui::GetWindowHeight()}, true);
        ImGui::TextUnformatted(message);
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered())
            ShowTooltip(message, &ctx.theme.fonts);
        ImGui::PopStyleColor();
    }

    [[nodiscard]] bool DrawFooterActions(SettingsState &st, EditorSettingsService &settings, const EditorGuiContext &ctx,
                                         const float centeredActionY, float &actionsStartX) {
        const auto prefab = st.prefab.owner.lock();
        const bool project = static_cast<SettingsTab>(st.activeTab) == SettingsTab::ProjectConfiguration;
        constexpr float actionH = 32.0F;
        const auto restoreDefaults = SettingsModal::kTextScope.Text(ctx.localization, "restore_defaults");
        const auto cancelLabel = SettingsModal::kTextScope.Text(ctx.localization, "cancel") + "###settings_cancel";
        const auto applyLabel = SettingsModal::kTextScope.Text(ctx.localization, "apply") + "###settings_apply";
        const auto &tokens = GetActiveTokens();
        const float scale = tokens.sizes.uiScale;
        const auto metrics = DesignSystem::MetricsFor(tokens, ComponentSize::Small);
        ScopedTextStyle text(ctx.theme.fonts.sansCompact, metrics.fontSize, FontPx::SansCompact);
        const auto width = [&](const char *label, const float minimum) {
            return std::max(minimum, (ImGui::CalcTextSize(label, nullptr, true).x + metrics.paddingX * 2.0F) / scale);
        };
        const float restoreW = width(restoreDefaults.c_str(), 124.0F);
        const float cancelW = width(cancelLabel.c_str(), 78.0F);
        const float applyW = width(applyLabel.c_str(), 70.0F);
        constexpr float gap = 8.0F;
        constexpr float footerPaddingX = 22.0F;
        const float actionsW = (restoreW + cancelW + applyW) * scale + gap * 2.0F;
        actionsStartX = ImGui::GetWindowWidth() - footerPaddingX - actionsW;
        ImGui::SetCursorPos({actionsStartX, centeredActionY});
        bool requestClose = false;
        if (Button({.label = restoreDefaults.c_str(),
                    .size = {restoreW, actionH},
                    .variant = ButtonVariant::Secondary,
                    .enabled = !project || static_cast<bool>(st.projectConfiguration),
                    .font = ctx.theme.fonts.sansCompact,
                    .baseFontSize = FontPx::SansCompact,
                    .componentSize = ComponentSize::Small})) {
            RestoreDefaultDraft(st, settings);
        }
        ImGui::SameLine(0.0F, gap);
        if (Button({.label = cancelLabel.c_str(),
                    .size = {cancelW, actionH},
                    .variant = ButtonVariant::Secondary,
                    .font = ctx.theme.fonts.sansCompact,
                    .baseFontSize = FontPx::SansCompact,
                    .componentSize = ComponentSize::Small})) {
            LOG_INFO("editor.settings", "Settings cancelled by user (dirty=%s).", st.dirty ? "yes" : "no");
            requestClose = true;
        }
        ImGui::SameLine(0.0F, gap);
        if (Button({.label = applyLabel.c_str(),
                    .size = {applyW, actionH},
                    .variant = ButtonVariant::Primary,
                    .enabled = project ? static_cast<bool>(st.projectConfiguration) : (!prefab || !prefab->IsDirty()),
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
        const float centeredActionY = (ImGui::GetWindowHeight() - actionH * GetActiveTokens().sizes.uiScale) * 0.5F;
        float actionsStartX{};
        const bool requestClose = DrawFooterActions(st, settings, ctx, centeredActionY, actionsStartX);
        DrawFooterStatus(st, ctx, footerPaddingX, actionsStartX);
        return requestClose;
    }

}  // namespace Horo::Editor::SettingsModalInternal
