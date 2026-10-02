#include "editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.h"

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
        enum class Permission : unsigned char {
            Read,
            Mutation,
        };

        enum class Status : unsigned char {
            Done,
            Approval,
            Denied,
        };

        struct AuditRow {
            std::string_view time;
            std::string_view tool;
            Permission permission;
            std::string_view requestKey;
            Status status;
        };

        constexpr std::array AuditRows{
            AuditRow{"12:08:14", "scene.query", Permission::Read, "workspace.global_dock.mcp.request.scene_query", Status::Done},
            AuditRow{"12:08:08", "asset.list", Permission::Read, "workspace.global_dock.mcp.request.asset_list", Status::Done},
            AuditRow{"12:07:51", "scene.move", Permission::Mutation, "workspace.global_dock.mcp.request.scene_move", Status::Approval},
            AuditRow{"12:07:39", "build.trigger", Permission::Mutation, "workspace.global_dock.mcp.request.build_trigger", Status::Denied},
        };

        [[nodiscard]] const char *PermissionKey(const Permission permission) noexcept {
            return permission == Permission::Read ? "workspace.global_dock.mcp.permission.read"
                                                  : "workspace.global_dock.mcp.permission.mutation";
        }

        [[nodiscard]] const char *StatusKey(const Status status) noexcept {
            using enum Status;
            switch (status) {
                case Done:
                    return "workspace.global_dock.mcp.status.done";
                case Approval:
                    return "workspace.global_dock.mcp.status.approval";
                case Denied:
                    return "workspace.global_dock.mcp.status.denied";
            }
            return "workspace.global_dock.mcp.status.denied";
        }

        [[nodiscard]] GlobalDockTone StatusTone(const Status status) noexcept {
            using enum GlobalDockTone;
            using enum Status;
            switch (status) {
                case Done:
                    return Positive;
                case Approval:
                    return Warning;
                case Denied:
                    return Error;
            }
            return Neutral;
        }

        [[nodiscard]] bool MatchesAuditSearch(const AuditRow &row, const std::string_view permission, const std::string_view request,
                                              const std::string_view status, const std::string_view search) {
            return GlobalDockContainsCaseInsensitive(row.tool, search) || GlobalDockContainsCaseInsensitive(permission, search) ||
                   GlobalDockContainsCaseInsensitive(request, search) || GlobalDockContainsCaseInsensitive(status, search);
        }

        [[nodiscard]] bool MatchesAuditFilter(const AuditRow &row, const bool all, const bool mutations, const bool errors) noexcept {
            return all || (mutations && row.permission == Permission::Mutation) || (errors && row.status == Status::Denied);
        }

        [[nodiscard]] float SessionControlWidth(const EditorGuiContext &context) {
            const auto &localization = context.localization;
            const auto &fonts = context.theme.fonts;
            const float scale = Theme::GetActiveTokens().sizes.uiScale;
            const float currentWidth = MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Label(),
                                                                  localization.Get("editor", "workspace.global_dock.mcp.session.current"));
            const float lastHourWidth =
                MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Label(),
                                           localization.Get("editor", "workspace.global_dock.mcp.session.last_hour"));
            return std::max(128.0F * scale, std::max(currentWidth, lastHourWidth) + 32.0F * scale);
        }
    }  // namespace

    struct GlobalDockMcpPane::TableLayout {
        float time;
        float tool;
        float permission;
        float request;
        float status;
    };

    void GlobalDockMcpPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        DrawTable(regions.contentOrigin, regions.contentWidth, regions.contentHeight, context);
    }

    void GlobalDockMcpPane::DrawToolbar(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
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

        const std::string &searchHint = localized("workspace.global_dock.mcp.search");
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##McpSearch", m_search, searchHint, fonts);
        DrawFilterActions(x, controlY, context);
        DrawGlobalDockToolbarSeparator(x, controlY);
        x += metrics.toolbarGap + Theme::GetActiveTokens().sizes.uiScale;
        DrawSessionActions(x, controlY, context);
    }

    float GlobalDockMcpPane::MeasureToolbarActions(const EditorGuiContext &context) const {
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        const GlobalDockToolbarChipProps all{.id = "McpAll",
                                             .label = localization.Get("editor", "workspace.global_dock.mcp.filter.all"),
                                             .count = 18U};
        const GlobalDockToolbarChipProps mutations{.id = "McpMutations",
                                                   .label = localization.Get("editor", "workspace.global_dock.mcp.filter.mutations"),
                                                   .count = 3U};
        const GlobalDockToolbarChipProps errors{.id = "McpErrors",
                                                .label = localization.Get("editor", "workspace.global_dock.mcp.filter.errors"),
                                                .count = 1U};
        const GlobalDockToolbarChipProps pause{.id = "McpPause",
                                               .label = localization.Get("editor", m_paused ? "workspace.global_dock.mcp.resume"
                                                                                            : "workspace.global_dock.mcp.pause"),
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const GlobalDockToolbarChipProps exportAudit{.id = "McpExport",
                                                     .label = localization.Get("editor", "workspace.global_dock.mcp.export"),
                                                     .icon = Ui::UiIcon::Download};
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        return SessionControlWidth(context) + MeasureGlobalDockToolbarChip(all, fonts) + MeasureGlobalDockToolbarChip(mutations, fonts) +
               MeasureGlobalDockToolbarChip(errors, fonts) + MeasureGlobalDockToolbarChip(pause, fonts) +
               MeasureGlobalDockToolbarChip(exportAudit, fonts) + metrics.toolbarGap * 7.0F + Theme::GetActiveTokens().sizes.uiScale;
    }

    void GlobalDockMcpPane::DrawFilterActions(float &x, const float y, const EditorGuiContext &context) {
        using enum Filter;
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        const GlobalDockToolbarChipProps all{.id = "McpAll",
                                             .label = localization.Get("editor", "workspace.global_dock.mcp.filter.all"),
                                             .count = 18U,
                                             .active = m_filter == All};
        const GlobalDockToolbarChipProps mutations{.id = "McpMutations",
                                                   .label = localization.Get("editor", "workspace.global_dock.mcp.filter.mutations"),
                                                   .count = 3U,
                                                   .tone = GlobalDockTone::Warning,
                                                   .active = m_filter == Mutations};
        const GlobalDockToolbarChipProps errors{.id = "McpErrors",
                                                .label = localization.Get("editor", "workspace.global_dock.mcp.filter.errors"),
                                                .count = 1U,
                                                .tone = GlobalDockTone::Error,
                                                .active = m_filter == Errors};
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float allWidth = MeasureGlobalDockToolbarChip(all, fonts);
        const float mutationWidth = MeasureGlobalDockToolbarChip(mutations, fonts);
        const float errorWidth = MeasureGlobalDockToolbarChip(errors, fonts);
        if (DrawGlobalDockToolbarChip({x, y}, allWidth, all, fonts))
            m_filter = All;
        x += allWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({x, y}, mutationWidth, mutations, fonts))
            m_filter = Mutations;
        x += mutationWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({x, y}, errorWidth, errors, fonts))
            m_filter = Errors;
        x += errorWidth + metrics.toolbarGap;
    }

    void GlobalDockMcpPane::DrawSessionActions(float x, const float y, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        const auto &localization = context.localization;
        const float sessionWidth = SessionControlWidth(context);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const GlobalDockToolbarChipProps pause{.id = "McpPause",
                                               .label = localization.Get("editor", m_paused ? "workspace.global_dock.mcp.resume"
                                                                                            : "workspace.global_dock.mcp.pause"),
                                               .active = m_paused,
                                               .icon = m_paused ? Ui::UiIcon::Play : Ui::UiIcon::Pause};
        const GlobalDockToolbarChipProps exportAudit{.id = "McpExport",
                                                     .label = localization.Get("editor", "workspace.global_dock.mcp.export"),
                                                     .icon = Ui::UiIcon::Download};
        const float pauseWidth = MeasureGlobalDockToolbarChip(pause, fonts);
        const float exportWidth = MeasureGlobalDockToolbarChip(exportAudit, fonts);
        const std::array<std::string, 2> sessionLabels{localization.Get("editor", "workspace.global_dock.mcp.session.current"),
                                                       localization.Get("editor", "workspace.global_dock.mcp.session.last_hour")};
        const std::array<const char *, 2> sessionItems{sessionLabels[0].c_str(), sessionLabels[1].c_str()};
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(sessionWidth);
        static_cast<void>(Ui::ComboControl("McpSession", &m_sessionSelection, sessionItems.data(), static_cast<int>(sessionItems.size()),
                                           fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += sessionWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({x, y}, pauseWidth, pause, fonts))
            m_paused = !m_paused;
        x += pauseWidth + metrics.toolbarGap;
        static_cast<void>(DrawGlobalDockToolbarChip({x, y}, exportWidth, exportAudit, fonts));
    }

    void GlobalDockMcpPane::DrawTable(const ImVec2 &contentOrigin, const float contentWidth, const float contentHeight,
                                      const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto localized = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const ImVec2 headerMin = contentOrigin;
        DrawGlobalDockTableHeaderSurface(headerMin, contentWidth, metrics.tableHeaderHeight);
        const TableLayout layout{.time = headerMin.x + metrics.contentPadding,
                                 .tool = headerMin.x + metrics.contentPadding + 72.0F * scale + metrics.columnGap,
                                 .permission = headerMin.x + metrics.contentPadding + 190.0F * scale + metrics.columnGap * 2.0F,
                                 .request = headerMin.x + metrics.contentPadding + 290.0F * scale + metrics.columnGap * 3.0F,
                                 .status = headerMin.x + contentWidth - metrics.contentPadding - 90.0F * scale};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              localized(key).c_str());
        };
        headerText(layout.time, "workspace.global_dock.mcp.column.time");
        headerText(layout.tool, "workspace.global_dock.mcp.column.tool");
        headerText(layout.permission, "workspace.global_dock.mcp.column.permission");
        headerText(layout.request, "workspace.global_dock.mcp.column.request");
        headerText(layout.status, "workspace.global_dock.mcp.column.status");

        const ImVec2 rowsOrigin{contentOrigin.x, contentOrigin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, contentHeight - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##McpRows", {contentWidth, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        for (std::size_t index = 0; index < AuditRows.size(); ++index)
            DrawAuditRow(index, contentWidth, layout, context);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void GlobalDockMcpPane::DrawAuditRow(const std::size_t index, const float width, const TableLayout &layout,
                                         const EditorGuiContext &context) {
        const AuditRow &row = AuditRows[index];
        const std::string permission = context.localization.Get("editor", PermissionKey(row.permission));
        const std::string request = context.localization.Get("editor", row.requestKey);
        const std::string status = context.localization.Get("editor", StatusKey(row.status));
        const std::string_view search{m_search.data()};
        const bool filterMatches =
            MatchesAuditFilter(row, m_filter == Filter::All, m_filter == Filter::Mutations, m_filter == Filter::Errors);
        if (const bool searchMatches = MatchesAuditSearch(row, permission, request, status, search); !filterMatches || !searchMatches)
            return;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const float rowHeight = metrics.tableRowHeight;
        ImGui::PushID(static_cast<int>(index));
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##audit-row", {width, rowHeight});
        DrawGlobalDockTableRowSurface(rowMin, width, rowHeight, ImGui::IsItemHovered());
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float textY = rowMin.y + (rowHeight - Theme::TextPx::Label()) * 0.5F;
        const float bottom = rowMin.y + rowHeight;
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.time, textY},
                                  {layout.tool - metrics.columnGap, bottom}, Theme::Muted(), row.time);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.tool, textY},
                                  {layout.permission - metrics.columnGap, bottom}, Theme::Accent(), row.tool);
        const ImVec4 permissionColor =
            row.permission == Permission::Mutation ? GlobalDockToneColor(GlobalDockTone::Warning) : Theme::Text();
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.permission, textY},
                                  {layout.request - metrics.columnGap, bottom}, permissionColor, permission);
        DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {layout.request, textY},
                                  {layout.status - metrics.columnGap, bottom}, Theme::Text(), request);
        static_cast<void>(DrawGlobalDockStatePill({layout.status, rowMin.y + (rowHeight - 22.0F * scale) * 0.5F}, status,
                                                  StatusTone(row.status), context.theme.fonts));
        ImGui::PopID();
    }

}  // namespace Horo::Editor
