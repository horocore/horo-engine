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

    struct PluginCardLayout {
        ImVec2 position;
        float width;
        float height;
        float innerX;
    };

    PluginCardLayout DrawPluginCardSurface(SettingsState &st, const PluginSpec &plugin) {
        const bool active = st.selectedPlugin == plugin.idx;
        const ImVec2 cardPos = ImGui::GetCursorScreenPos();
        const float cardW = ImGui::GetContentRegionAvail().x;
        constexpr float cardH = 96.0F;
        constexpr float padLeft = 14.0F;
        const float innerX = cardPos.x + padLeft + 24.0F;
        auto *drawList = ImGui::GetWindowDrawList();

        if (active) {
            drawList->AddRectFilled(cardPos, {cardPos.x + cardW, cardPos.y + cardH}, ImColor{Accent().x, Accent().y, Accent().z, 0.09F},
                                    Layout::Radius);
            drawList->AddRect(cardPos, {cardPos.x + cardW, cardPos.y + cardH}, U32(BorderStrong()), Layout::Radius);
            drawList->AddRectFilled(cardPos, {cardPos.x + 3.0F, cardPos.y + cardH}, U32(Accent()), 1.0F);
        } else if (ImGui::IsMouseHoveringRect(cardPos, {cardPos.x + cardW, cardPos.y + cardH})) {
            drawList->AddRectFilled(cardPos, {cardPos.x + cardW, cardPos.y + cardH}, U32(Hover()), Layout::Radius);
            drawList->AddRect(cardPos, {cardPos.x + cardW, cardPos.y + cardH}, U32(Border()), Layout::Radius);
        } else {
            drawList->AddRect(cardPos, {cardPos.x + cardW, cardPos.y + cardH}, U32(Border()), Layout::Radius);
        }

        ImGui::InvisibleButton("##card", {cardW, cardH});
        if (ImGui::IsItemClicked()) {
            st.selectedPlugin = plugin.idx;
            st.pluginDetailTab[plugin.idx] = 0;
        }
        return {.position = cardPos, .width = cardW, .height = cardH, .innerX = innerX};
    }

    void DrawPluginCardDetails(const EditorGuiContext &ctx, const PluginSpec &plugin, const PluginCardLayout &card) {
        const bool enabled = *plugin.enabled;
        const ImVec2 dotCenter{card.position.x + 20.0F, card.position.y + 18.0F};
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddCircleFilled(dotCenter, 4.5F, enabled ? U32(Ok()) : U32(Dim()));
        if (enabled)
            drawList->AddCircleFilled(dotCenter, 7.0F, ImColor{Ok().x, Ok().y, Ok().z, 0.13F});

        ImGui::SetCursorScreenPos({card.innerX, card.position.y + 9.0F});
        {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Label(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Text());
            ImGui::TextUnformatted(plugin.name);
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos({card.position.x + card.width - 48.0F, card.position.y + 9.0F});
        (void)ToggleControl("##tog", plugin.enabled, ctx.theme.fonts, false);

        ImGui::SetCursorScreenPos({card.innerX, card.position.y + 33.0F});
        {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card.width - (card.innerX - card.position.x) - 56.0F);
            ImGui::TextWrapped("%s", plugin.desc);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos({card.innerX, card.position.y + card.height - 26.0F});
        Badge({.label = plugin.version, .tone = BadgeTone::Accent}, ctx.theme.fonts);
        Badge({.label = plugin.statusLabel, .tone = plugin.statusTone}, ctx.theme.fonts);
        {
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Dim());
            ImGui::TextUnformatted(plugin.category);
            ImGui::PopStyleColor();
        }
    }

    void DrawPluginCard(SettingsState &st, const EditorGuiContext &ctx, const PluginSpec &plugin) {
        ImGui::PushID(plugin.idx);
        const PluginCardLayout card = DrawPluginCardSurface(st, plugin);
        DrawPluginCardDetails(ctx, plugin, card);
        ImGui::SetCursorScreenPos({card.position.x, card.position.y + card.height + 8.0F});
        ImGui::PopID();
    }

    void DrawPluginFilter(SettingsState &st, const EditorGuiContext &ctx) {
        SettingGroup("INSTALLED PLUGINS", ctx.theme.fonts, true);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0F, 7.0F});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Bg3());
        ImGui::PushStyleColor(ImGuiCol_Text, Text());
        st.pluginFilter.resize(std::min(st.pluginFilter.size(), std::size_t{63}));
        st.pluginFilter.resize(63, '\0');
        ImGui::InputTextWithHint("##filter", "Filter plugins...", st.pluginFilter.data(), st.pluginFilter.size() + 1);
        st.pluginFilter.resize(st.pluginFilter.find('\0'));
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::Dummy({0.0F, 10.0F});
    }

    void DrawPluginList(SettingsState &st, const EditorGuiContext &ctx, float /*listW*/) {  // NOSONAR(cpp:S1144)
        DrawPluginFilter(st, ctx);

        const std::string mcpDesc = ctx.localization.Get("editor", "settings.plugins.mcp.desc");
        const std::string mcpStatus = ctx.localization.Get("editor", "settings.plugins.status.trusted");
        const std::string mcpCat = ctx.localization.Get("editor", "settings.plugins.category.editor");

        const std::string fmodDesc = ctx.localization.Get("editor", "settings.plugins.fmod.desc");
        const std::string fmodStatus = ctx.localization.Get("editor", "settings.plugins.status.vendor");
        const std::string fmodCat = ctx.localization.Get("editor", "settings.plugins.category.audio");

        const std::string steamDesc = ctx.localization.Get("editor", "settings.plugins.steam.desc");
        const std::string steamStatus = ctx.localization.Get("editor", "settings.plugins.status.disabled");
        const std::string steamCat = ctx.localization.Get("editor", "settings.plugins.category.platform");

        const std::array<PluginSpec, 3> kPlugins = {{
            {"Horo MCP Bridge", mcpDesc.c_str(), "v0.4.0", mcpStatus.c_str(), BadgeTone::Success, mcpCat.c_str(), 0,
             &st.plugins.horoMcpBridge},
            {"Vendor FMOD Integration", fmodDesc.c_str(), "v2.02.20", fmodStatus.c_str(), BadgeTone::Success, fmodCat.c_str(), 1,
             &st.plugins.fmodIntegration},
            {"Steamworks SDK", steamDesc.c_str(), "v1.59", steamStatus.c_str(), BadgeTone::Warning, steamCat.c_str(), 2,
             &st.plugins.steamworksSdk},
        }};

        for (const auto &p : kPlugins) {
            if (!ContainsCaseInsensitive(p.name, st.pluginFilter))
                continue;
            DrawPluginCard(st, ctx, p);
        }
    }

}  // namespace Horo::Editor::SettingsModalInternal
