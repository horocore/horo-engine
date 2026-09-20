#pragma once

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/SettingsModal.h"

#include <span>
#include <string>
#include <string_view>

namespace Horo::Editor::SettingsModalInternal {
    namespace Layout {
        constexpr float ModalW = 960.0F;
        constexpr float ModalH = 680.0F;
        constexpr float ViewportPad = 48.0F;
        constexpr float HeaderH = 57.0F;
        constexpr float FooterH = Theme::Layout::FooterH;
        constexpr float NavW = 200.0F;
        constexpr float Radius = 4.0F;
    }  // namespace Layout

    enum class SettingsTab : int {
        General = 0,
        Appearance,
        Input,
        Rendering,
        Audio,
        Network,
        Packages,
        Diagnostics,
        Plugins,
    };

    struct NavItem {
        const char *label;
        const char *icon;
        SettingsTab tab;
    };

    struct PluginSpec {
        const char *name;
        const char *desc;
        const char *version;
        const char *statusLabel;
        Ui::BadgeTone statusTone;
        const char *category;
        int idx;
        bool *enabled;
    };

    struct PluginDetailHeaderSpec {
        const char *name;
        const char *desc;
        const char *scopeBadge;
        const char *signedBadge;
        Ui::BadgeTone signedTone;
        const char *restartBadge;
        const char *action1;
        const char *action2;
    };

    struct PermissionRowSpec {
        const char *icon;
        const char *title;
        const char *desc;
        const char *badgeText;
        Ui::BadgeTone badgeTone;
    };

    struct DiagnosticMetricSpec {
        const char *label;
        const char *value;
        const char *hint;
        ImVec4 valueColour;
    };

    [[nodiscard]] bool ContainsCaseInsensitive(const char *text, const std::string &query);

    void DrawNavigationContent(SettingsState &st, const EditorGuiContext &ctx);
    void DrawContent(SettingsState &st, const EditorGuiContext &ctx);
    [[nodiscard]] bool DrawFooterContent(SettingsState &st, EditorSettingsService &settings, const EditorGuiContext &ctx);

    void DrawPlugins(SettingsState &st, const EditorGuiContext &ctx);
    void DrawPluginSectionTabs(SettingsState &st, const EditorGuiContext &ctx);
    void DrawActiveTabIndicator();
    void DrawPluginList(SettingsState &st, const EditorGuiContext &ctx, float listW);
    void DrawPluginDetailPanel(SettingsState &st, const EditorGuiContext &ctx, float w, bool embedded = false);
    void DrawExtensionManager(SettingsState &st, const EditorGuiContext &ctx);

    void DrawPermissionRows(std::span<const PermissionRowSpec> rows, const EditorGuiContext &ctx);
    void DrawDiagnosticMetrics(std::span<const DiagnosticMetricSpec> metrics, const EditorGuiContext &ctx);
    void DrawDiagnosticActivity(std::span<const char *const> items, const EditorGuiContext &ctx);
    void DrawManifestBlock(const char *path, const char *manifest, const EditorGuiContext &ctx);
}  // namespace Horo::Editor::SettingsModalInternal
