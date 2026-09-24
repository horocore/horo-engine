#include "editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.h"

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
#include <utility>

namespace Horo::Editor {
    namespace {
        struct NetworkRow {
            std::string_view connection;
            const char *stateKey;
            GlobalDockTone stateTone;
            std::string_view rtt;
            std::string_view traffic;
            const char *queueKey;
        };

        constexpr std::array Rows{
            NetworkRow{"local-server:7777", "workspace.global_dock.network.state.open", GlobalDockTone::Positive, "42 ms", "15.4 kb/s",
                       "workspace.global_dock.network.queue.local"},
            NetworkRow{"editor-presence", "workspace.global_dock.network.state.open", GlobalDockTone::Positive, "18 ms", "1.2 kb/s",
                       "workspace.global_dock.network.queue.presence"},
            NetworkRow{"asset-sync", "workspace.global_dock.network.state.idle", GlobalDockTone::Warning, "—", "0 kb/s",
                       "workspace.global_dock.network.queue.asset"},
        };

    }  // namespace

    struct GlobalDockNetworkPane::TableLayout {
        float connection;
        float state;
        float rtt;
        float traffic;
        float queue;
    };

    void GlobalDockNetworkPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        const float metricHeight = DrawMetrics(regions.contentOrigin, regions.contentWidth, context);
        DrawTable({regions.contentOrigin.x, regions.contentOrigin.y + metricHeight}, regions.contentWidth,
                  std::max(1.0F, regions.contentHeight - metricHeight), context);
    }

    void GlobalDockNetworkPane::DrawToolbar(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };

        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        const float fixedWidth = MeasureToolbarActions(context);
        const float searchWidth = ResolveGlobalDockSearchWidth(regions.toolbarWidth, fixedWidth);
        float x = regions.toolbarOrigin.x + metrics.toolbarPaddingX;

        const std::string &searchHint = localized("workspace.global_dock.network.search");
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##NetworkSearch", m_search, searchHint, fonts);
        DrawToolbarActions(x, controlY, context);
    }

    float GlobalDockNetworkPane::MeasureToolbarActions(const EditorGuiContext &context) const {
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        const std::array props{GlobalDockToolbarChipProps{.id = "NetworkConnections",
                                                          .label = localization.Get("editor", "workspace.global_dock.network.connections")},
                               GlobalDockToolbarChipProps{.id = "NetworkQueues",
                                                          .label = localization.Get("editor", "workspace.global_dock.network.queues")},
                               GlobalDockToolbarChipProps{.id = "NetworkTrace",
                                                          .label = localization.Get("editor", "workspace.global_dock.network.trace")},
                               GlobalDockToolbarChipProps{.id = "NetworkPause",
                                                          .label =
                                                              localization.Get("editor",
                                                                               m_paused ? "workspace.global_dock.network.resume_capture"
                                                                                        : "workspace.global_dock.network.pause_capture"),
                                                          .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause},
                               GlobalDockToolbarChipProps{.id = "NetworkClear",
                                                          .label = localization.Get("editor", "workspace.global_dock.network.clear"),
                                                          .icon = Ui::UiIcon::ClearAll}};
        float width = 144.0F * Theme::GetActiveTokens().sizes.uiScale;
        for (const auto &propsItem : props)
            width += MeasureGlobalDockToolbarChip(propsItem, fonts);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        return width + metrics.toolbarGap * 7.0F + Theme::GetActiveTokens().sizes.uiScale;
    }

    void GlobalDockNetworkPane::DrawToolbarActions(float x, const float y, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const std::array views{GlobalDockToolbarChipProps{.id = "NetworkConnections",
                                                          .label = localized("workspace.global_dock.network.connections"),
                                                          .active = m_viewSelection == 0},
                               GlobalDockToolbarChipProps{.id = "NetworkQueues",
                                                          .label = localized("workspace.global_dock.network.queues"),
                                                          .active = m_viewSelection == 1},
                               GlobalDockToolbarChipProps{.id = "NetworkTrace",
                                                          .label = localized("workspace.global_dock.network.trace"),
                                                          .active = m_viewSelection == 2}};
        DrawSessionSelector(x, y, context);
        for (std::size_t index = 0; index < views.size(); ++index) {
            const float viewWidth = MeasureGlobalDockToolbarChip(views[index], fonts);
            if (DrawGlobalDockToolbarChip({x, y}, viewWidth, views[index], fonts))
                m_viewSelection = static_cast<int>(index);
            x += viewWidth + metrics.toolbarGap;
        }
        DrawGlobalDockToolbarSeparator(x, y);
        x += metrics.toolbarGap + scale;
        const GlobalDockToolbarChipProps pause{.id = "NetworkPause",
                                               .label = localized(m_paused ? "workspace.global_dock.network.resume_capture"
                                                                           : "workspace.global_dock.network.pause_capture"),
                                               .tone = GlobalDockTone::Accent,
                                               .active = true,
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const float pauseWidth = MeasureGlobalDockToolbarChip(pause, fonts);
        if (DrawGlobalDockToolbarChip({x, y}, pauseWidth, pause, fonts))
            m_paused = !m_paused;
        x += pauseWidth + metrics.toolbarGap;
        const GlobalDockToolbarChipProps clear{.id = "NetworkClear",
                                               .label = localized("workspace.global_dock.network.clear"),
                                               .icon = Ui::UiIcon::ClearAll};
        if (DrawGlobalDockToolbarChip({x, y}, MeasureGlobalDockToolbarChip(clear, fonts), clear, fonts))
            m_cleared = true;
    }

    void GlobalDockNetworkPane::DrawSessionSelector(float &x, const float y, const EditorGuiContext &context) {
        const float width = 144.0F * Theme::GetActiveTokens().sizes.uiScale;
        const std::array<std::string, 2> labels{context.localization.Get("editor", "workspace.global_dock.network.session.current"),
                                                context.localization.Get("editor", "workspace.global_dock.network.session.all")};
        const std::array<const char *, 2> items{labels[0].c_str(), labels[1].c_str()};
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(width);
        static_cast<void>(Ui::ComboControl("NetworkSession", &m_sessionSelection, items.data(), static_cast<int>(items.size()),
                                           context.theme.fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += width + ResolveGlobalDockPaneMetrics().toolbarGap;
    }

    float GlobalDockNetworkPane::DrawMetrics(const ImVec2 &origin, const float width, const EditorGuiContext &context) const {
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const std::array<GlobalDockMetricCardProps, 4> cards{
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.network.metric.rtt"), .value = "42 ms"},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.network.metric.receive"), .value = "12.3", .note = "kb/s"},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.network.metric.send"), .value = "3.1", .note = "kb/s"},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.network.metric.loss"),
                                      .value = "0.2%",
                                      .valueTone = GlobalDockTone::Positive},
        };
        return DrawGlobalDockMetricGrid(origin, width, cards, context.theme.fonts);
    }

    void GlobalDockNetworkPane::DrawTable(const ImVec2 &origin, const float width, const float height,
                                          const EditorGuiContext &context) const {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = origin;
        DrawGlobalDockTableHeaderSurface(headerMin, width, metrics.tableHeaderHeight);
        const TableLayout layout{.connection = headerMin.x + metrics.contentPadding,
                                 .state = headerMin.x + metrics.contentPadding + 126.0F * scale + metrics.columnGap,
                                 .rtt = headerMin.x + metrics.contentPadding + 210.0F * scale + metrics.columnGap * 2.0F,
                                 .traffic = headerMin.x + metrics.contentPadding + 300.0F * scale + metrics.columnGap * 3.0F,
                                 .queue = headerMin.x + metrics.contentPadding + 390.0F * scale + metrics.columnGap * 4.0F};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              localized(key).c_str());
        };
        headerText(layout.connection, "workspace.global_dock.network.column.connection");
        headerText(layout.state, "workspace.global_dock.network.column.state");
        headerText(layout.rtt, "workspace.global_dock.network.column.rtt");
        headerText(layout.traffic, "workspace.global_dock.network.column.traffic");
        headerText(layout.queue, "workspace.global_dock.network.column.queues");

        const ImVec2 rowsOrigin{headerMin.x, headerMin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, height - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##NetworkRows", {width, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        if (!m_cleared) {
            for (std::size_t index = 0; index < Rows.size(); ++index)
                DrawRow(index, width, layout, context);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockNetworkPane::DrawRow(const std::size_t index, const float width, const TableLayout &layout,
                                        const EditorGuiContext &context) const {
        const NetworkRow &row = Rows[index];
        const std::string &state = context.localization.Get("editor", row.stateKey);
        const std::string &queue = context.localization.Get("editor", row.queueKey);
        if (const std::string_view search{m_search.data()}; !GlobalDockContainsCaseInsensitive(row.connection, search) &&
                                                            !GlobalDockContainsCaseInsensitive(state, search) &&
                                                            !GlobalDockContainsCaseInsensitive(queue, search))
            return;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##network-row", {width, metrics.tableRowHeight});
        DrawGlobalDockTableRowSurface(rowMin, width, metrics.tableRowHeight, ImGui::IsItemHovered());
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const float bottom = rowMin.y + metrics.tableRowHeight;
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.connection, textY},
                                  {layout.state - metrics.columnGap, bottom}, Theme::Text(), row.connection);
        static_cast<void>(DrawGlobalDockStatePill({layout.state, rowMin.y + (metrics.tableRowHeight - 22.0F * scale) * 0.5F}, state,
                                                  row.stateTone, context.theme.fonts));
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.rtt, textY},
                                  {layout.traffic - metrics.columnGap, bottom}, Theme::Text(), row.rtt);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.traffic, textY},
                                  {layout.queue - metrics.columnGap, bottom}, Theme::Text(), row.traffic);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.queue, textY},
                                  {rowMin.x + width - metrics.contentPadding, bottom}, Theme::Text(), queue);
        ImGui::PopID();
    }

}  // namespace Horo::Editor
