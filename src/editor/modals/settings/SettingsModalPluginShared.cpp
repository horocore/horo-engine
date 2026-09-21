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

    void DrawActiveTabIndicator() {
        auto *drawList = ImGui::GetWindowDrawList();
        const ImVec2 bMin = ImGui::GetItemRectMin();
        const ImVec2 bMax = ImGui::GetItemRectMax();
        drawList->AddLine({bMin.x + 8.0F, bMax.y - 2.0F}, {bMax.x - 8.0F, bMax.y - 2.0F}, U32(Accent()), 2.0F);
    }

    void DrawPluginSectionTabs(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 2> sectionTabsStr = {ctx.localization.Get("editor", "settings.plugins.installed"),
                                                           ctx.localization.Get("editor", "settings.extensions.marketplace")};
        const std::array<const char *, 2> kSectionTabs = {sectionTabsStr[0].c_str(), sectionTabsStr[1].c_str()};
        constexpr float pad = 4.0F;
        constexpr float tabH = 31.0F;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float containerW = std::min(520.0F, availableWidth);
        const float installedW = 210.0F;
        const float runtimeW = containerW - installedW - pad * 3.0F;
        const float containerH = tabH + pad * 2.0F;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto *dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(p, {p.x + containerW, p.y + containerH}, U32(Bg0()), Layout::Radius);
        dl->AddRect(p, {p.x + containerW, p.y + containerH}, U32(Border()), Layout::Radius);
        ImGui::SetCursorScreenPos({p.x + pad, p.y + pad});

        for (int i = 0; i < 2; ++i) {
            if (i > 0)
                ImGui::SameLine(0.0F, 4.0F);
            const bool active = st.pluginSectionTab == i;
            const float tabW = i == 0 ? installedW : runtimeW;
            ImGui::PushID(i + 100);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Layout::Radius);
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  active ? ImVec4{Accent().x, Accent().y, Accent().z, 0.12F} : ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ImVec4{Accent().x, Accent().y, Accent().z, 0.18F} : Hover());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{Accent().x, Accent().y, Accent().z, 0.22F});
            ImGui::PushStyleColor(ImGuiCol_Text, active ? Text() : Muted());
            {
                ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Label(), FontPx::Sans);
                if (ImGui::Button(kSectionTabs[i], {tabW, tabH}))
                    st.pluginSectionTab = i;
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::PopID();

            if (active)
                DrawActiveTabIndicator();
        }

        ImGui::SetCursorScreenPos({p.x, p.y + containerH + 12.0F});
    }

    void DrawPlugins(SettingsState &st, const EditorGuiContext &ctx) {
        DrawExtensionManager(st, ctx);
    }

    void DrawPluginCardBackground(const ImVec2 position, const float width, const float height) {
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(position, {position.x + width, position.y + height}, U32(Bg3()), Layout::Radius);
        drawList->AddRect(position, {position.x + width, position.y + height}, U32(Border()), Layout::Radius);
    }

    void DrawPermissionRows(const std::span<const PermissionRowSpec> rows, const EditorGuiContext &ctx) {
        for (const auto &perm : rows) {
            ImGui::PushID(perm.title);
            const float cardW = ImGui::GetContentRegionAvail().x;
            constexpr float cardH = 66.0F;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const BadgeProps badge{
                .label = perm.badgeText,
                .tone = perm.badgeTone,
            };
            const float badgeW = Ui::BadgeWidth(badge, ctx.theme.fonts);
            DrawPluginCardBackground(p, cardW, cardH);

            ImGui::SetCursorScreenPos({p.x + 13.0F, p.y + 19.0F});
            {
                ScopedTextStyle ts(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
                ImGui::PushStyleColor(ImGuiCol_Text, BadgeToneColor(perm.badgeTone));
                ImGui::TextUnformatted(perm.icon);
                ImGui::PopStyleColor();
            }

            ImGui::SetCursorScreenPos({p.x + 40.0F, p.y + 11.0F});
            {
                ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, Text());
                ImGui::TextUnformatted(perm.title);
                ImGui::PopStyleColor();
            }

            ImGui::SetCursorScreenPos({p.x + 40.0F, p.y + 32.0F});
            {
                ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, Muted());
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - badgeW - 68.0F);
                ImGui::TextWrapped("%s", perm.desc);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }

            ImGui::SetCursorScreenPos({p.x + cardW - badgeW - 14.0F, p.y + 20.0F});
            Badge(badge, ctx.theme.fonts);

            ImGui::SetCursorScreenPos({p.x, p.y + cardH + 8.0F});
            ImGui::PopID();
        }
    }

    void DrawDiagnosticMetrics(const std::span<const DiagnosticMetricSpec> metrics, const EditorGuiContext &ctx) {
        constexpr float gap = 8.0F;
        constexpr float cardH = 68.0F;
        const float availW = ImGui::GetContentRegionAvail().x;
        const float cardW = (availW - gap * static_cast<float>(metrics.size() - 1)) / static_cast<float>(metrics.size());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (std::size_t i = 0; i < metrics.size(); ++i) {
            const ImVec2 p{start.x + static_cast<float>(i) * (cardW + gap), start.y};
            const auto &m = metrics[i];
            DrawPluginCardBackground(p, cardW, cardH);

            ImGui::SetCursorScreenPos({p.x + 12.0F, p.y + 10.0F});
            {
                ScopedTextStyle ts(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
                ImGui::PushStyleColor(ImGuiCol_Text, Dim());
                ImGui::TextUnformatted(m.label);
                ImGui::PopStyleColor();
            }

            ImGui::SetCursorScreenPos({p.x + 12.0F, p.y + 28.0F});
            {
                ScopedTextStyle ts(ctx.theme.fonts.sansEmphasis, TextPx::Body(), FontPx::SansEmphasis);
                ImGui::PushStyleColor(ImGuiCol_Text, m.valueColour);
                ImGui::TextUnformatted(m.value);
                ImGui::PopStyleColor();
            }

            if (m.hint != nullptr && m.hint[0] != '\0') {
                ImGui::SetCursorScreenPos({p.x + 12.0F, p.y + 49.0F});
                ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, Dim());
                ImGui::TextUnformatted(m.hint);
                ImGui::PopStyleColor();
            }
        }

        ImGui::SetCursorScreenPos({start.x, start.y + cardH + 12.0F});
    }

    void DrawDiagnosticActivity(const std::span<const char *const> items, const EditorGuiContext &ctx) {
        SettingGroup("RECENT ACTIVITY", ctx.theme.fonts);
        for (std::size_t i = 0; i < items.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const float rowW = ImGui::GetContentRegionAvail().x;
            constexpr float rowH = 30.0F;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            auto *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, {p.x + rowW, p.y + rowH}, U32(i % 2 == 0 ? Bg3() : Bg2()), Layout::Radius);

            ImGui::SetCursorScreenPos({p.x + 10.0F, p.y + 7.0F});
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextUnformatted(items[i]);
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos({p.x, p.y + rowH + 4.0F});
            ImGui::PopID();
        }
    }

    void DrawManifestBlock(const char *path, const char *manifest, const EditorGuiContext &ctx) {
        const ImVec2 headerPos = ImGui::GetCursorScreenPos();
        const float headerW = ImGui::GetContentRegionAvail().x;

        FieldLabel("MANIFEST", ctx.theme.fonts);
        if (path != nullptr && path[0] != '\0') {
            ImGui::SameLine(0.0F, 8.0F);
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Dim());
            ImGui::TextUnformatted(path);
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos({headerPos.x + headerW - 58.0F, headerPos.y - 2.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{8.0F, 3.0F});
        if (ImGui::Button("Copy", {58.0F, 24.0F}))
            ImGui::SetClipboardText(manifest);
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos({headerPos.x, headerPos.y + 30.0F});

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg0());
        ImGui::BeginChild("manifest-code", {0.0F, 168.0F}, true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextUnformatted(manifest);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    [[nodiscard]] bool ContainsCaseInsensitive(const char *text, const std::string &query) {
        if (query.empty())
            return true;
        for (const char *start = text; *start != '\0'; ++start) {
            const char *candidate = start;
            const char *needle = query.c_str();
            while (*candidate != '\0' && *needle != '\0' &&
                   std::tolower(static_cast<unsigned char>(*candidate)) == std::tolower(static_cast<unsigned char>(*needle))) {
                ++candidate;
                ++needle;
            }
            if (*needle == '\0')
                return true;
        }
        return false;
    }

}  // namespace Horo::Editor::SettingsModalInternal
