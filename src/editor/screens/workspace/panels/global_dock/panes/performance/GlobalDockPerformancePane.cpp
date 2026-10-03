#include "editor/screens/workspace/panels/global_dock/panes/performance/GlobalDockPerformancePane.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <array>
#include <string>

namespace Horo::Editor {
    namespace {
        struct PerformanceRow {
            const char *subsystemKey;
            std::string_view p50;
            std::string_view p95;
            const char *notesKey;
        };

        constexpr std::array FrameSamples{0.31F, 0.40F, 0.37F, 0.64F, 0.45F, 0.78F, 0.49F, 0.61F, 0.39F, 0.68F, 0.51F};
        constexpr std::array CpuSamples{0.24F, 0.31F, 0.21F, 0.43F, 0.36F, 0.49F, 0.34F, 0.45F, 0.39F};
        constexpr std::array GpuSamples{0.35F, 0.42F, 0.64F, 0.45F, 0.74F, 0.54F, 0.63F, 0.42F, 0.67F, 0.58F};
        constexpr std::array MemorySamples{0.21F, 0.23F, 0.28F, 0.32F, 0.37F, 0.45F, 0.49F, 0.53F, 0.57F};
        constexpr std::array Rows{
            PerformanceRow{"workspace.global_dock.performance.subsystem.renderer", "8.2 ms", "15.8 ms",
                           "workspace.global_dock.performance.notes.renderer"},
            PerformanceRow{"workspace.global_dock.performance.subsystem.physics", "0.72 ms", "1.14 ms",
                           "workspace.global_dock.performance.notes.physics"},
            PerformanceRow{"workspace.global_dock.performance.subsystem.audio", "0.18 ms", "0.31 ms",
                           "workspace.global_dock.performance.notes.audio"},
        };

        void DrawMetricGridSurface(const ImVec2 origin, const float width, const float height, const float scale) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockContentSurface()));
            drawList->AddLine({origin.x, origin.y + height - scale}, {origin.x + width, origin.y + height - scale},
                              Theme::U32(Theme::Border()));
        }
    }  // namespace

    struct GlobalDockPerformancePane::TableLayout {
        float subsystem;
        float p50;
        float p95;
        float notes;
    };

    void GlobalDockPerformancePane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        const float metricHeight = DrawMetrics(regions.contentOrigin, regions.contentWidth, context);
        DrawTable({regions.contentOrigin.x, regions.contentOrigin.y + metricHeight}, regions.contentWidth,
                  std::max(1.0F, regions.contentHeight - metricHeight), context);
    }

    void GlobalDockPerformancePane::DrawToolbar(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
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
        const GlobalDockToolbarChipProps live{.id = "PerformanceLive",
                                              .label = localized("workspace.global_dock.performance.live"),
                                              .tone = GlobalDockTone::Positive,
                                              .active = m_live};
        const GlobalDockToolbarChipProps capture{.id = "PerformanceCapture",
                                                 .label = localized("workspace.global_dock.performance.capture"),
                                                 .tone = GlobalDockTone::Accent,
                                                 .active = true,
                                                 .icon = Ui::UiIcon::Record};
        const float liveWidth = MeasureGlobalDockToolbarChip(live, fonts);
        const float captureWidth = MeasureGlobalDockToolbarChip(capture, fonts);
        const float windowWidth = 148.0F * scale;
        const float subsystemWidth = 142.0F * scale;
        const float fixedWidth = windowWidth + subsystemWidth + liveWidth + captureWidth + metrics.toolbarGap * 5.0F + scale;
        const float searchWidth = std::max(180.0F * scale, regions.toolbarWidth - metrics.toolbarPaddingX * 2.0F - fixedWidth);
        float x = regions.toolbarOrigin.x + metrics.toolbarPaddingX;

        const std::string &searchHint = localized("workspace.global_dock.performance.search");
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##PerformanceSearch", m_search, searchHint, fonts);

        DrawToolbarSelectors(x, controlY, context);
        DrawGlobalDockToolbarSeparator(x, controlY);
        x += metrics.toolbarGap + scale;
        if (DrawGlobalDockToolbarChip({x, controlY}, liveWidth, live, fonts))
            m_live = !m_live;
        x += liveWidth + metrics.toolbarGap;
        static_cast<void>(DrawGlobalDockToolbarChip({x, controlY}, captureWidth, capture, fonts));
    }

    void GlobalDockPerformancePane::DrawToolbarSelectors(float &x, const float y, const EditorGuiContext &context) {
        const std::array<std::string, 3> windowLabels{context.localization.Get("editor",
                                                                               "workspace.global_dock.performance.window.ten_seconds"),
                                                      context.localization.Get("editor", "workspace.global_dock.performance.window.minute"),
                                                      context.localization.Get("editor",
                                                                               "workspace.global_dock.performance.window.five_minutes")};
        const std::array<const char *, 3> windowItems{windowLabels[0].c_str(), windowLabels[1].c_str(), windowLabels[2].c_str()};
        const float windowWidth = 148.0F * Theme::GetActiveTokens().sizes.uiScale;
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(windowWidth);
        static_cast<void>(Ui::ComboControl("PerformanceWindow", &m_windowSelection, windowItems.data(),
                                           static_cast<int>(windowItems.size()), context.theme.fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += windowWidth + ResolveGlobalDockPaneMetrics().toolbarGap;
        const std::array<std::string, 4>
            subsystemLabels{context.localization.Get("editor", "workspace.global_dock.performance.subsystem.all"),
                            context.localization.Get("editor", "workspace.global_dock.performance.subsystem.renderer"),
                            context.localization.Get("editor", "workspace.global_dock.performance.subsystem.physics"),
                            context.localization.Get("editor", "workspace.global_dock.performance.subsystem.audio")};
        const std::array<const char *, 4> subsystemItems{subsystemLabels[0].c_str(), subsystemLabels[1].c_str(), subsystemLabels[2].c_str(),
                                                         subsystemLabels[3].c_str()};
        const float subsystemWidth = 142.0F * Theme::GetActiveTokens().sizes.uiScale;
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(subsystemWidth);
        static_cast<void>(Ui::ComboControl("PerformanceSubsystem", &m_subsystemSelection, subsystemItems.data(),
                                           static_cast<int>(subsystemItems.size()), context.theme.fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += subsystemWidth + ResolveGlobalDockPaneMetrics().toolbarGap;
    }

    float GlobalDockPerformancePane::DrawMetrics(const ImVec2 &contentOrigin, const float contentWidth,
                                                 const EditorGuiContext &context) const {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };

        const bool narrow = contentWidth < 900.0F * scale;
        const int columns = narrow ? 2 : 4;
        const float gridPadding = 10.0F * scale;
        const float cardGap = 8.0F * scale;
        const float cardHeight = 106.0F * scale;
        const int rows = narrow ? 2 : 1;
        const float gridHeight = gridPadding * 2.0F + cardHeight * static_cast<float>(rows) + cardGap * static_cast<float>(rows - 1);
        const float cardWidth = std::max(120.0F * scale, (contentWidth - gridPadding * 2.0F - cardGap * static_cast<float>(columns - 1)) /
                                                             static_cast<float>(columns));
        DrawMetricGridSurface(contentOrigin, contentWidth, gridHeight, scale);
        const std::array<GlobalDockMetricCardProps, 4> cards{
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.performance.metric.frame_time"),
                                      .value = "16.7 ms",
                                      .note = localized("workspace.global_dock.performance.metric.frame_budget"),
                                      .noteTone = GlobalDockTone::Warning,
                                      .samples = FrameSamples,
                                      .budget = 0.52F},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.performance.metric.cpu"),
                                      .value = "4.1 ms",
                                      .valueTone = GlobalDockTone::Positive,
                                      .samples = CpuSamples},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.performance.metric.gpu"),
                                      .value = "15.8 ms",
                                      .valueTone = GlobalDockTone::Warning,
                                      .samples = GpuSamples},
            GlobalDockMetricCardProps{.label = localized("workspace.global_dock.performance.metric.memory"),
                                      .value = "628 MB",
                                      .note = "+8.4",
                                      .samples = MemorySamples},
        };
        for (std::size_t index = 0; index < cards.size(); ++index) {
            const int column = static_cast<int>(index) % columns;
            const int row = static_cast<int>(index) / columns;
            DrawGlobalDockMetricCard({contentOrigin.x + gridPadding + static_cast<float>(column) * (cardWidth + cardGap),
                                      contentOrigin.y + gridPadding + static_cast<float>(row) * (cardHeight + cardGap)},
                                     {cardWidth, cardHeight}, cards[index], fonts);
        }
        return gridHeight;
    }

    void GlobalDockPerformancePane::DrawTable(const ImVec2 &origin, const float width, const float height,
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
        const TableLayout layout{.subsystem = headerMin.x + metrics.contentPadding,
                                 .p50 = headerMin.x + metrics.contentPadding + 68.0F * scale + metrics.columnGap,
                                 .p95 = headerMin.x + metrics.contentPadding + 142.0F * scale + metrics.columnGap * 2.0F,
                                 .notes = headerMin.x + metrics.contentPadding + 238.0F * scale + metrics.columnGap * 3.0F};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              localized(key).c_str());
        };
        headerText(layout.subsystem, "workspace.global_dock.performance.column.subsystem");
        headerText(layout.p50, "workspace.global_dock.performance.column.p50");
        headerText(layout.p95, "workspace.global_dock.performance.column.p95");
        headerText(layout.notes, "workspace.global_dock.performance.column.notes");

        const ImVec2 rowsOrigin{headerMin.x, headerMin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, height - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##PerformanceRows", {width, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        for (std::size_t index = 0; index < Rows.size(); ++index)
            DrawRow(index, width, layout, context);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockPerformancePane::DrawRow(const std::size_t index, const float width, const TableLayout &layout,
                                            const EditorGuiContext &context) const {
        if (m_subsystemSelection != 0 && static_cast<std::size_t>(m_subsystemSelection - 1) != index)
            return;
        const PerformanceRow &row = Rows[index];
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##performance-row", {width, metrics.tableRowHeight});
        DrawGlobalDockTableRowSurface(origin, width, metrics.tableRowHeight, ImGui::IsItemHovered());
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float textY = origin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const float bottom = origin.y + metrics.tableRowHeight;
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.subsystem, textY},
                                  {layout.p50 - metrics.columnGap, bottom}, Theme::Text(),
                                  context.localization.Get("editor", row.subsystemKey));
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.p50, textY},
                                  {layout.p95 - metrics.columnGap, bottom}, Theme::Text(), row.p50);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.p95, textY},
                                  {layout.notes - metrics.columnGap, bottom}, Theme::Text(), row.p95);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.notes, textY},
                                  {origin.x + width - metrics.contentPadding, bottom}, Theme::Text(),
                                  context.localization.Get("editor", row.notesKey));
        ImGui::PopID();
    }

}  // namespace Horo::Editor
