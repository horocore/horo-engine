#include "editor/screens/workspace/panels/global_dock/panes/physics/GlobalDockPhysicsPane.h"

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
        struct PhysicsRow {
            std::string_view body;
            const char *stateKey;
            GlobalDockTone stateTone;
            const char *shapeKey;
            const char *layerKey;
            const char *timingKey;
        };

        constexpr std::array Rows{
            PhysicsRow{"Player", "state.active", GlobalDockTone::Positive, "shape.capsule", "layer.player", "timing.player"},
            PhysicsRow{"PF Enemy", "state.active", GlobalDockTone::Positive, "shape.convex", "layer.enemy", "timing.enemy"},
            PhysicsRow{"Floor 000", "state.static", GlobalDockTone::Neutral, "shape.mesh", "layer.terrain", "timing.floor"},
        };

    }  // namespace

    struct GlobalDockPhysicsPane::TableLayout {
        float body;
        float state;
        float shape;
        float layer;
        float timing;
    };

    void GlobalDockPhysicsPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        const float metricHeight = DrawMetrics(regions.contentOrigin, regions.contentWidth, context);
        DrawTable({regions.contentOrigin.x, regions.contentOrigin.y + metricHeight}, regions.contentWidth,
                  std::max(1.0F, regions.contentHeight - metricHeight), context);
    }

    void GlobalDockPhysicsPane::DrawToolbar(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        const auto localized = [&](const char *key) -> const std::string & {
            return GlobalDockPhysicsPane::kTextScope.Text(context.localization, key);
        };

        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        const float fixedWidth = MeasureToolbarActions(context);
        const float searchWidth = ResolveGlobalDockSearchWidth(regions.toolbarWidth, fixedWidth);
        float x = regions.toolbarOrigin.x + metrics.toolbarPaddingX;

        const std::string &searchHint = localized("search");
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##PhysicsSearch", m_search, searchHint, fonts);
        DrawWorldSelector(x, controlY, context);
        DrawToolbarActions(x, controlY, context);
    }

    float GlobalDockPhysicsPane::MeasureToolbarActions(const EditorGuiContext &context) const {
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        const std::array props{GlobalDockToolbarChipProps{.id = "PhysicsColliders",
                                                          .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "colliders")},
                               GlobalDockToolbarChipProps{.id = "PhysicsContacts",
                                                          .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "contacts")},
                               GlobalDockToolbarChipProps{.id = "PhysicsConstraints",
                                                          .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "constraints")},
                               GlobalDockToolbarChipProps{.id = "PhysicsPause",
                                                          .label = GlobalDockPhysicsPane::kTextScope.Text(localization,
                                                                                                          m_paused ? "resume" : "pause"),
                                                          .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause},
                               GlobalDockToolbarChipProps{.id = "PhysicsStep",
                                                          .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "step"),
                                                          .icon = Ui::UiIcon::Play}};
        float width = 132.0F * Theme::GetActiveTokens().sizes.uiScale;
        for (const auto &propsItem : props)
            width += MeasureGlobalDockToolbarChip(propsItem, fonts);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        return width + metrics.toolbarGap * 7.0F + Theme::GetActiveTokens().sizes.uiScale;
    }

    void GlobalDockPhysicsPane::DrawWorldSelector(float &x, const float y, const EditorGuiContext &context) {
        const float width = 132.0F * Theme::GetActiveTokens().sizes.uiScale;
        const std::array<std::string, 2> labels{GlobalDockPhysicsPane::kTextScope.Text(context.localization, "world.room"),
                                                GlobalDockPhysicsPane::kTextScope.Text(context.localization, "world.all")};
        const std::array<const char *, 2> items{labels[0].c_str(), labels[1].c_str()};
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(width);
        static_cast<void>(Ui::ComboControl("PhysicsWorld", &m_worldSelection, items.data(), static_cast<int>(items.size()),
                                           context.theme.fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += width + ResolveGlobalDockPaneMetrics().toolbarGap;
    }

    void GlobalDockPhysicsPane::DrawToolbarActions(float x, const float y, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        std::array props{GlobalDockToolbarChipProps{.id = "PhysicsColliders",
                                                    .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "colliders"),
                                                    .active = m_colliders},
                         GlobalDockToolbarChipProps{.id = "PhysicsContacts",
                                                    .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "contacts"),
                                                    .active = m_contacts},
                         GlobalDockToolbarChipProps{.id = "PhysicsConstraints",
                                                    .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "constraints"),
                                                    .active = m_constraints}};
        std::array<bool *, 3> values{&m_colliders, &m_contacts, &m_constraints};
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        for (std::size_t index = 0; index < props.size(); ++index) {
            const float width = MeasureGlobalDockToolbarChip(props[index], fonts);
            if (DrawGlobalDockToolbarChip({x, y}, width, props[index], fonts))
                *values[index] = !*values[index];
            x += width + metrics.toolbarGap;
        }
        DrawGlobalDockToolbarSeparator(x, y);
        x += metrics.toolbarGap + Theme::GetActiveTokens().sizes.uiScale;
        const GlobalDockToolbarChipProps pause{.id = "PhysicsPause",
                                               .label = GlobalDockPhysicsPane::kTextScope.Text(localization, m_paused ? "resume" : "pause"),
                                               .tone = GlobalDockTone::Accent,
                                               .active = true,
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const float pauseWidth = MeasureGlobalDockToolbarChip(pause, fonts);
        if (DrawGlobalDockToolbarChip({x, y}, pauseWidth, pause, fonts))
            m_paused = !m_paused;
        x += pauseWidth + metrics.toolbarGap;
        const GlobalDockToolbarChipProps step{.id = "PhysicsStep",
                                              .label = GlobalDockPhysicsPane::kTextScope.Text(localization, "step"),
                                              .disabled = !m_paused,
                                              .icon = Ui::UiIcon::Play};
        static_cast<void>(DrawGlobalDockToolbarChip({x, y}, MeasureGlobalDockToolbarChip(step, fonts), step, fonts));
    }

    float GlobalDockPhysicsPane::DrawMetrics(const ImVec2 &origin, const float width, const EditorGuiContext &context) const {
        const auto localized = [&](const char *key) -> const std::string & {
            return GlobalDockPhysicsPane::kTextScope.Text(context.localization, key);
        };
        const std::array<GlobalDockMetricCardProps, 4> cards{
            GlobalDockMetricCardProps{.label = localized("metric.step"), .value = "0.72 ms", .valueTone = GlobalDockTone::Positive},
            GlobalDockMetricCardProps{.label = localized("metric.active_sleeping"), .value = "28", .note = "/ 64"},
            GlobalDockMetricCardProps{.label = localized("metric.contacts_pairs"), .value = "14", .note = "/ 37"},
            GlobalDockMetricCardProps{.label = localized("metric.snapshot"),
                                      .value = "8 / 16",
                                      .note = localized("metric.dropped"),
                                      .noteTone = GlobalDockTone::Positive},
        };
        return DrawGlobalDockMetricGrid(origin, width, cards, context.theme.fonts);
    }

    void GlobalDockPhysicsPane::DrawTable(const ImVec2 &origin, const float width, const float height,
                                          const EditorGuiContext &context) const {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto localized = [&](const char *key) -> const std::string & {
            return GlobalDockPhysicsPane::kTextScope.Text(context.localization, key);
        };
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = origin;
        DrawGlobalDockTableHeaderSurface(headerMin, width, metrics.tableHeaderHeight);
        const TableLayout layout{.body = headerMin.x + metrics.contentPadding,
                                 .state = headerMin.x + metrics.contentPadding + 110.0F * scale + metrics.columnGap,
                                 .shape = headerMin.x + metrics.contentPadding + 200.0F * scale + metrics.columnGap * 2.0F,
                                 .layer = headerMin.x + metrics.contentPadding + 290.0F * scale + metrics.columnGap * 3.0F,
                                 .timing = headerMin.x + metrics.contentPadding + 390.0F * scale + metrics.columnGap * 4.0F};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              localized(key).c_str());
        };
        headerText(layout.body, "column.body");
        headerText(layout.state, "column.state");
        headerText(layout.shape, "column.shape");
        headerText(layout.layer, "column.layer");
        headerText(layout.timing, "column.timing");

        const ImVec2 rowsOrigin{headerMin.x, headerMin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, height - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##PhysicsRows", {width, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        for (std::size_t index = 0; index < Rows.size(); ++index)
            DrawRow(index, width, layout, context);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockPhysicsPane::DrawRow(const std::size_t index, const float width, const TableLayout &layout,
                                        const EditorGuiContext &context) const {
        const PhysicsRow &row = Rows[index];
        const std::string &state = GlobalDockPhysicsPane::kTextScope.Text(context.localization, row.stateKey);
        const std::string &shape = GlobalDockPhysicsPane::kTextScope.Text(context.localization, row.shapeKey);
        const std::string &layer = GlobalDockPhysicsPane::kTextScope.Text(context.localization, row.layerKey);
        const std::string &timing = GlobalDockPhysicsPane::kTextScope.Text(context.localization, row.timingKey);
        if (const std::string_view search{m_search.data()};
            !GlobalDockContainsCaseInsensitive(row.body, search) && !GlobalDockContainsCaseInsensitive(state, search) &&
            !GlobalDockContainsCaseInsensitive(shape, search) && !GlobalDockContainsCaseInsensitive(layer, search))
            return;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##physics-row", {width, metrics.tableRowHeight});
        DrawGlobalDockTableRowSurface(origin, width, metrics.tableRowHeight, ImGui::IsItemHovered());
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float textY = origin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const float bottom = origin.y + metrics.tableRowHeight;
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.body, textY},
                                  {layout.state - metrics.columnGap, bottom}, Theme::Text(), row.body);
        static_cast<void>(DrawGlobalDockStatePill({layout.state, origin.y + (metrics.tableRowHeight - 22.0F * scale) * 0.5F}, state,
                                                  row.stateTone, context.theme.fonts));
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.shape, textY},
                                  {layout.layer - metrics.columnGap, bottom}, Theme::Text(), shape);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.layer, textY},
                                  {layout.timing - metrics.columnGap, bottom}, Theme::Text(), layer);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.timing, textY},
                                  {origin.x + width - metrics.contentPadding, bottom}, Theme::Text(), timing);
        ImGui::PopID();
    }

}  // namespace Horo::Editor
