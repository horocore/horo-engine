#include "editor/screens/workspace/panels/global_dock/panes/audio/GlobalDockAudioPane.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Horo::Editor {
    namespace {
        struct AudioRow {
            const char *busKey;
            std::string_view gain;
            float level;
            std::string_view voices;
        };

        constexpr std::array Rows{
            AudioRow{"workspace.global_dock.audio.bus.master", "0.0 dB", 0.78F, "12"},
            AudioRow{"workspace.global_dock.audio.bus.music", "−6.0 dB", 0.58F, "2"},
            AudioRow{"workspace.global_dock.audio.bus.sfx", "−1.5 dB", 0.69F, "7"},
            AudioRow{"workspace.global_dock.audio.bus.ambient", "−9.0 dB", 0.39F, "3"},
        };

    }  // namespace

    struct GlobalDockAudioPane::TableLayout {
        float bus;
        float gain;
        float level;
        float voices;
        float controls;
    };

    void GlobalDockAudioPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        const float metricHeight = DrawMetrics(regions.contentOrigin, regions.contentWidth, context);
        DrawBusTable({regions.contentOrigin.x, regions.contentOrigin.y + metricHeight}, regions.contentWidth,
                     std::max(1.0F, regions.contentHeight - metricHeight), context);
    }

    void GlobalDockAudioPane::DrawToolbar(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };

        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        const float deviceWidth = 138.0F * scale;
        const float fixedWidth = deviceWidth + MeasureToolbarActions(context);
        const float searchWidth = ResolveGlobalDockSearchWidth(regions.toolbarWidth, fixedWidth);
        float x = regions.toolbarOrigin.x + metrics.toolbarPaddingX;

        const std::string &searchHint = localized("workspace.global_dock.audio.search");
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##AudioSearch", m_search, searchHint, fonts);
        const std::array<std::string, 2> deviceLabels{localized("workspace.global_dock.audio.device.default"),
                                                      localized("workspace.global_dock.audio.device.headphones")};
        const std::array<const char *, 2> deviceItems{deviceLabels[0].c_str(), deviceLabels[1].c_str()};
        ImGui::SetCursorScreenPos({x, controlY});
        ImGui::SetNextItemWidth(deviceWidth);
        static_cast<void>(Ui::ComboControl("AudioDevice", &m_deviceSelection, deviceItems.data(), static_cast<int>(deviceItems.size()),
                                           fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += deviceWidth + metrics.toolbarGap;
        DrawToolbarActions(x, controlY, context);
    }

    float GlobalDockAudioPane::MeasureToolbarActions(const EditorGuiContext &context) const {
        const auto &fonts = context.theme.fonts;
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const GlobalDockToolbarChipProps meters{.id = "AudioMeters", .label = localized("workspace.global_dock.audio.meters")};
        const GlobalDockToolbarChipProps voices{.id = "AudioVoices", .label = localized("workspace.global_dock.audio.voices")};
        const GlobalDockToolbarChipProps pause{.id = "AudioPause",
                                               .label = localized(m_paused ? "workspace.global_dock.audio.resume"
                                                                           : "workspace.global_dock.audio.pause"),
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const GlobalDockToolbarChipProps mute{.id = "AudioMuteAll",
                                              .label = localized(m_allMuted ? "workspace.global_dock.audio.unmute_all"
                                                                            : "workspace.global_dock.audio.mute_all"),
                                              .icon = Ui::UiIcon::VolumeOff};
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        return MeasureGlobalDockToolbarChip(meters, fonts) + MeasureGlobalDockToolbarChip(voices, fonts) +
               MeasureGlobalDockToolbarChip(pause, fonts) + MeasureGlobalDockToolbarChip(mute, fonts) + metrics.toolbarGap * 6.0F +
               Theme::GetActiveTokens().sizes.uiScale;
    }

    void GlobalDockAudioPane::DrawToolbarActions(float x, const float y, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const GlobalDockToolbarChipProps meters{.id = "AudioMeters",
                                                .label = localized("workspace.global_dock.audio.meters"),
                                                .active = m_viewSelection == 0};
        const GlobalDockToolbarChipProps voices{.id = "AudioVoices",
                                                .label = localized("workspace.global_dock.audio.voices"),
                                                .active = m_viewSelection == 1};
        const GlobalDockToolbarChipProps pause{.id = "AudioPause",
                                               .label = localized(m_paused ? "workspace.global_dock.audio.resume"
                                                                           : "workspace.global_dock.audio.pause"),
                                               .active = m_paused,
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const GlobalDockToolbarChipProps muteAll{.id = "AudioMuteAll",
                                                 .label = localized(m_allMuted ? "workspace.global_dock.audio.unmute_all"
                                                                               : "workspace.global_dock.audio.mute_all"),
                                                 .tone = GlobalDockTone::Error,
                                                 .active = m_allMuted,
                                                 .toneLabel = true,
                                                 .icon = Ui::UiIcon::VolumeOff};
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float metersWidth = MeasureGlobalDockToolbarChip(meters, fonts);
        const float voicesWidth = MeasureGlobalDockToolbarChip(voices, fonts);
        const float pauseWidth = MeasureGlobalDockToolbarChip(pause, fonts);
        const float muteWidth = MeasureGlobalDockToolbarChip(muteAll, fonts);
        if (DrawGlobalDockToolbarChip({x, y}, metersWidth, meters, fonts))
            m_viewSelection = 0;
        x += metersWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({x, y}, voicesWidth, voices, fonts))
            m_viewSelection = 1;
        x += voicesWidth + metrics.toolbarGap;
        DrawGlobalDockToolbarSeparator(x, y);
        x += metrics.toolbarGap + Theme::GetActiveTokens().sizes.uiScale;
        if (DrawGlobalDockToolbarChip({x, y}, pauseWidth, pause, fonts))
            m_paused = !m_paused;
        x += pauseWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({x, y}, muteWidth, muteAll, fonts)) {
            m_allMuted = !m_allMuted;
            m_muted.fill(m_allMuted);
        }
    }

    float GlobalDockAudioPane::DrawMetrics(const ImVec2 &origin, const float width, const EditorGuiContext &context) const {
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const std::array<GlobalDockMetricCardProps, 4> cards{
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.audio.metric.master_peak"), .value = "−3.2 dB"},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.audio.metric.voices"), .value = "12 / 64"},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.audio.metric.callback"),
                                      .value = "0.31 ms",
                                      .valueTone = GlobalDockTone::Positive},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.audio.metric.underruns"),
                                      .value = "0",
                                      .valueTone = GlobalDockTone::Positive},
        };
        return DrawGlobalDockMetricGrid(origin, width, cards, context.theme.fonts);
    }

    void GlobalDockAudioPane::DrawBusTable(const ImVec2 &origin, const float width, const float height, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = origin;
        DrawGlobalDockTableHeaderSurface(headerMin, width, metrics.tableHeaderHeight);
        const TableLayout layout{.bus = headerMin.x + metrics.contentPadding,
                                 .gain = headerMin.x + metrics.contentPadding + 110.0F * scale + metrics.columnGap,
                                 .level = headerMin.x + metrics.contentPadding + 180.0F * scale + metrics.columnGap * 2.0F,
                                 .voices = headerMin.x + width - metrics.contentPadding - 198.0F * scale - metrics.columnGap,
                                 .controls = headerMin.x + width - metrics.contentPadding - 108.0F * scale};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              localized(key).c_str());
        };
        headerText(layout.bus, "workspace.global_dock.audio.column.bus");
        headerText(layout.gain, "workspace.global_dock.audio.column.gain");
        headerText(layout.level, "workspace.global_dock.audio.column.level");
        headerText(layout.voices, "workspace.global_dock.audio.column.voices");
        headerText(layout.controls, "workspace.global_dock.audio.column.controls");

        const ImVec2 rowsOrigin{headerMin.x, headerMin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, height - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##AudioRows", {width, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        for (std::size_t index = 0; index < Rows.size(); ++index)
            DrawBusRow(index, width, layout, context);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockAudioPane::DrawBusRow(const std::size_t index, const float width, const TableLayout &layout,
                                         const EditorGuiContext &context) {
        const AudioRow &row = Rows[index];
        const std::string &bus = context.localization.Get("editor", row.busKey);
        if (!GlobalDockContainsCaseInsensitive(bus, std::string_view{m_search.data()}))
            return;
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##audio-row", {width, metrics.tableRowHeight});
        DrawGlobalDockTableRowSurface(rowMin, width, metrics.tableRowHeight, ImGui::IsItemHovered());
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const float rowBottom = rowMin.y + metrics.tableRowHeight;
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.bus, textY},
                                  {layout.gain - metrics.columnGap, rowBottom}, Theme::Text(), bus);
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.gain, textY},
                                  {layout.level - metrics.columnGap, rowBottom}, Theme::Text(), row.gain);
        DrawGlobalDockMeter({layout.level, rowMin.y + (metrics.tableRowHeight - 6.0F * scale) * 0.5F},
                            std::max(1.0F, layout.voices - metrics.columnGap - layout.level), m_muted[index] ? 0.0F : row.level);
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.voices, textY},
                                  {layout.controls - metrics.columnGap, rowBottom}, Theme::Text(), row.voices);
        const GlobalDockToolbarChipProps mute{.id = "AudioMute", .label = "M", .active = m_muted[index]};
        const GlobalDockToolbarChipProps solo{.id = "AudioSolo", .label = "S", .active = m_solo[index]};
        const float buttonWidth = 30.0F * scale;
        const float buttonY = rowMin.y + (metrics.tableRowHeight - metrics.controlHeight) * 0.5F;
        if (DrawGlobalDockToolbarChip({layout.controls, buttonY}, buttonWidth, mute, fonts))
            m_muted[index] = !m_muted[index];
        if (DrawGlobalDockToolbarChip({layout.controls + buttonWidth + 4.0F * scale, buttonY}, buttonWidth, solo, fonts))
            m_solo[index] = !m_solo[index];
        ImGui::PopID();
    }

}  // namespace Horo::Editor
