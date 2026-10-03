#include "editor/screens/workspace/panels/global_dock/panes/console/GlobalDockConsolePane.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        struct Layout {
            float scale;
            float toolbar;
            float control;
            float gap;
            float header;
            float row;
            float compactRow;
            float padding;
            float columnGap;
            float time;
            float level;
            float source;
            float icon;
        };

        enum class Severity : std::size_t {
            Info,
            Warning,
            Error
        };

        struct ToolbarButtonProps {
            const char *id;
            Ui::UiIcon icon;
            const char *label;
            std::optional<std::size_t> count;
            ImVec4 iconColor;
            bool active;
        };

        [[nodiscard]] Layout GetLayout() noexcept {
            const float s = Theme::GetActiveTokens().sizes.uiScale;
            const GlobalDockPaneMetrics pane = ResolveGlobalDockPaneMetrics();
            return {
                .scale = s,
                .toolbar = pane.toolbarHeight,
                .control = pane.controlHeight,
                .gap = pane.toolbarGap,
                .header = pane.tableHeaderHeight,
                .row = pane.tableRowHeight,
                .compactRow = 26.0F * s,
                .padding = pane.contentPadding,
                .columnGap = 8.0F * s,
                .time = 72.0F * s,
                .level = 112.0F * s,
                .source = 120.0F * s,
                .icon = 14.0F * s,
            };
        }

        [[nodiscard]] Severity GetSeverity(const Log::Level level) noexcept {
            if (level == Log::Level::Warn)
                return Severity::Warning;
            if (level == Log::Level::Error || level == Log::Level::Critical || level == Log::Level::Off)
                return Severity::Error;
            return Severity::Info;
        }

        [[nodiscard]] ImVec4 SeverityColor(const Severity severity) noexcept {
            if (severity == Severity::Warning)
                return Theme::ConsoleWarning();
            if (severity == Severity::Error)
                return Theme::ConsoleError();
            return Theme::ConsoleInfo();
        }

        [[nodiscard]] Ui::UiIcon SeverityIcon(const Severity severity) noexcept {
            if (severity == Severity::Warning)
                return Ui::UiIcon::Warning;
            if (severity == Severity::Error)
                return Ui::UiIcon::Error;
            return Ui::UiIcon::Info;
        }

        [[nodiscard]] const char *SeverityLabelKey(const Severity severity) noexcept {
            if (severity == Severity::Warning)
                return "workspace.global_dock.console.row.warning";
            if (severity == Severity::Error)
                return "workspace.global_dock.console.row.error";
            return "workspace.global_dock.console.row.info";
        }

        [[nodiscard]] bool ContainsInsensitive(const std::string_view text, const std::string_view query) {
            if (query.empty())
                return true;
            return !std::ranges::search(text, query, [](const char left, const char right) {
                return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
            }).empty();
        }

        [[nodiscard]] std::string TimeLabel(const std::chrono::system_clock::time_point timestamp) {
            const std::time_t value = std::chrono::system_clock::to_time_t(timestamp);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &value);
#else
            localtime_r(&value, &local);
#endif
            return std::format("{:02d}:{:02d}:{:02d}", local.tm_hour, local.tm_min, local.tm_sec);
        }

        [[nodiscard]] std::string SourceLabel(const Log::StructuredLogRecord &record) {
            const std::string_view category{record.category};
            std::string source{category.substr(0, category.find('.'))};
            if (source.empty())
                source = record.context.empty() ? "Editor" : record.context;
            source.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(source.front())));
            return source;
        }

        void ClippedText(ImDrawList &drawList, ImFont *font, const float size, const ImVec2 minimum, const ImVec2 maximum,
                         const ImVec4 color, const std::string_view text) {
            if (maximum.x <= minimum.x || text.empty())
                return;
            drawList.PushClipRect(minimum, maximum, true);
            drawList.AddText(font, size, minimum, Theme::U32(color), text.data(), text.data() + text.size());
            drawList.PopClipRect();
        }

        [[nodiscard]] float TextWidth(ImFont *font, const float size, const char *text) {
            return font != nullptr ? font->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0F, text).x
                                   : ImGui::CalcTextSize(text).x;
        }

        [[nodiscard]] float ToolbarButtonWidth(ImFont *font, const char *label, const Ui::UiIcon icon,
                                               const std::optional<std::size_t> count, const float minimumWidth) {
            const Layout l = GetLayout();
            const float fontSize = Theme::TextPx::Caption();
            float width = l.gap * 2.0F;
            if (icon != Ui::UiIcon::None)
                width += l.icon + l.gap * 0.7F;
            if (label != nullptr && label[0] != '\0')
                width += TextWidth(font, fontSize, label);
            if (count.has_value()) {
                const std::string countText = std::to_string(*count);
                const float badgeHeight = std::max(l.icon, fontSize + 4.0F * l.scale);
                width += l.gap * 0.7F + std::max(badgeHeight, TextWidth(font, fontSize, countText.c_str()) + l.gap);
            }
            return std::max(minimumWidth * l.scale, width);
        }

        [[nodiscard]] bool ToolbarButton(const ImVec2 minimum, const ImVec2 extent, const ToolbarButtonProps &props,
                                         const Theme::Fonts &fonts) {
            const Layout l = GetLayout();
            ImGui::SetCursorScreenPos(minimum);
            const bool pressed = ImGui::InvisibleButton(props.id, extent);
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImVec4 surface = hovered ? Theme::Hover() : Theme::BottomDockControlSurface();
            drawList->AddRectFilled(minimum, {minimum.x + extent.x, minimum.y + extent.y}, Theme::U32(surface),
                                    Theme::GetActiveTokens().radii.control);
            drawList->AddRect(minimum, {minimum.x + extent.x, minimum.y + extent.y},
                              Theme::U32(props.active ? Theme::Accent() : Theme::Border()), Theme::GetActiveTokens().radii.control);
            if (props.active) {
                const float inset = 1.0F * l.scale;
                drawList->AddRect({minimum.x + inset, minimum.y + inset}, {minimum.x + extent.x - inset, minimum.y + extent.y - inset},
                                  Theme::U32(Theme::AccentSoft()), std::max(0.0F, Theme::GetActiveTokens().radii.control - inset));
            }
            float x = minimum.x + l.gap;
            if (props.icon != Ui::UiIcon::None) {
                Ui::DrawEditorIcon(drawList, props.icon, {x, minimum.y + (extent.y - l.icon) * 0.5F}, {l.icon, l.icon},
                                   Theme::U32(props.iconColor), fonts.icon);
                x += l.icon + l.gap * 0.7F;
            }
            const float fontSize = Theme::TextPx::Caption();
            if (props.label != nullptr) {
                drawList->AddText(fonts.sansCompact, fontSize, {x, minimum.y + (extent.y - fontSize) * 0.5F},
                                  Theme::U32(props.active ? Theme::Text() : Theme::Muted()), props.label);
            }
            if (props.count.has_value()) {
                const std::string value = std::to_string(*props.count);
                const float badgeHeight = std::max(l.icon, fontSize + 4.0F * l.scale);
                const float badgeWidth = std::max(badgeHeight, TextWidth(fonts.sansCompact, fontSize, value.c_str()) + l.gap);
                const ImVec2 badge{minimum.x + extent.x - l.gap - badgeWidth, minimum.y + (extent.y - badgeHeight) * 0.5F};
                drawList->AddRectFilled(badge, {badge.x + badgeWidth, badge.y + badgeHeight}, Theme::U32(surface),
                                        Theme::GetActiveTokens().radii.control);
                const float countWidth = TextWidth(fonts.sansCompact, fontSize, value.c_str());
                drawList->AddText(fonts.sansCompact, fontSize,
                                  {badge.x + (badgeWidth - countWidth) * 0.5F, badge.y + (badgeHeight - fontSize) * 0.5F},
                                  Theme::U32(Theme::Dim()), value.c_str());
            }
            return pressed;
        }

        void Separator(const float x, const float y) {
            const Layout l = GetLayout();
            ImGui::GetWindowDrawList()->AddLine({x, y + 5.0F * l.scale}, {x, y + l.control - 5.0F * l.scale}, Theme::U32(Theme::Border()));
        }
    }  // namespace

    struct GlobalDockConsolePane::ToolbarLayout {
        bool narrow;
        float search;
        float all;
        float info;
        float warning;
        float error;
        float source;
        float collapse;
        float autoScroll;
        float clear;
    };

    /** @copydoc GlobalDockConsolePane::Attach */
    void GlobalDockConsolePane::Attach(const Log::IStructuredLogQuery *logQuery) noexcept {
        m_logQuery = logQuery;
        m_snapshot = {};
        m_revision = 0;
        m_clearedThroughSequence.reset();
        m_levelFilter = LevelFilter::All;
        m_search.fill('\0');
        m_sources.clear();
        m_selectedSource.clear();
        m_filteredIndices.clear();
        m_visibleLevelCounts.fill(0U);
        m_selectedSequence.reset();
        m_filterDirty = true;
        m_hasDrawnRows = false;
    }

    /** @copydoc GlobalDockConsolePane::Detach */
    void GlobalDockConsolePane::Detach() noexcept {
        m_logQuery = nullptr;
        m_snapshot = {};
        m_revision = 0;
        m_sources.clear();
        m_selectedSource.clear();
        m_filteredIndices.clear();
        m_selectedSequence.reset();
    }

    /** @copydoc GlobalDockConsolePane::Draw */
    void GlobalDockConsolePane::Draw(const ImVec2 &origin, const float width, const EditorGuiContext &context) {
        const bool changed = RefreshSnapshot();
        if (m_filterDirty)
            RebuildFilter();
        const Layout l = GetLayout();
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
        DrawToolbar(origin, width, context);
        DrawTableHeader({origin.x, origin.y + l.toolbar}, width, context);
        const float rowsY = origin.y + l.toolbar + l.header;
        const float rowsHeight = std::max(1.0F, bottom - rowsY);
        ImGui::SetCursorScreenPos({origin.x, rowsY});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##ConsoleRows", {width, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        const bool wasAtBottom = ImGui::GetScrollY() >= std::max(0.0F, ImGui::GetScrollMaxY() - 2.0F);
        DrawLogRows(width, rowsHeight, context);
        if (m_autoScroll && changed && m_hasDrawnRows && wasAtBottom)
            ImGui::SetScrollHereY(1.0F);
        m_hasDrawnRows = true;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockConsolePane::DrawToolbar(const ImVec2 &minimum, const float width, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        DrawGlobalDockToolbarSurface(minimum, width, l.toolbar);
        const ToolbarLayout toolbar = ResolveToolbarLayout(width, context);
        const float y = minimum.y + (l.toolbar - l.control) * 0.5F;
        float x = minimum.x + 10.0F * l.scale;
        DrawToolbarSearch(x, y, toolbar, context);
        DrawToolbarFilters(x, y, toolbar, context);
        Separator(x, y);
        x += l.gap * 2.0F;
        DrawToolbarSource(x, y, toolbar, context);
        Separator(x, y);
        x += l.gap * 2.0F;
        DrawToolbarActions(x, y, toolbar, context);
    }

    GlobalDockConsolePane::ToolbarLayout GlobalDockConsolePane::ResolveToolbarLayout(const float width,
                                                                                     const EditorGuiContext &context) const {
        const Layout l = GetLayout();
        const auto &fonts = context.theme.fonts;
        const bool narrow = width < 980.0F * l.scale;
        const std::string &allLabel = context.localization.Get("editor", "workspace.global_dock.console.filter.all");
        const std::string &infoLabel = context.localization.Get("editor", "workspace.global_dock.console.filter.info");
        const std::string &warningLabel = context.localization.Get("editor", "workspace.global_dock.console.filter.warn");
        const std::string &errorLabel = context.localization.Get("editor", "workspace.global_dock.console.filter.error");
        const std::string &collapseLabel = context.localization.Get("editor", "workspace.global_dock.console.collapse");
        const std::string &autoScrollLabel = context.localization.Get("editor", "workspace.global_dock.console.auto_scroll");
        const std::string &clearLabel = context.localization.Get("editor", "workspace.global_dock.console.clear");
        const float allWidth =
            narrow ? 46.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, allLabel.c_str(), Ui::UiIcon::None, m_matchingRecordCount, 54.0F);
        const float infoWidth =
            narrow ? 46.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, infoLabel.c_str(), Ui::UiIcon::Info, m_visibleLevelCounts[0], 80.0F);
        const float warningWidth =
            narrow ? 46.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, warningLabel.c_str(), Ui::UiIcon::Warning, m_visibleLevelCounts[1], 109.0F);
        const float errorWidth =
            narrow ? 46.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, errorLabel.c_str(), Ui::UiIcon::Error, m_visibleLevelCounts[2], 91.0F);
        const float sourceWidth = narrow ? 112.0F * l.scale : 128.0F * l.scale;
        const float actionWidth =
            narrow ? 34.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, collapseLabel.c_str(), Ui::UiIcon::ViewList, std::nullopt, 90.0F);
        const float autoWidth =
            narrow ? 34.0F * l.scale
                   : ToolbarButtonWidth(fonts.sansCompact, autoScrollLabel.c_str(), Ui::UiIcon::Check, std::nullopt, 100.0F);
        const float clearWidth =
            narrow ? 34.0F * l.scale : ToolbarButtonWidth(fonts.sansCompact, clearLabel.c_str(), Ui::UiIcon::Delete, std::nullopt, 68.0F);
        const float fixed =
            allWidth + infoWidth + warningWidth + errorWidth + sourceWidth + actionWidth + autoWidth + clearWidth + l.gap * 12.0F;
        const float searchWidth = std::max(120.0F * l.scale, width - 20.0F * l.scale - fixed);
        return {.narrow = narrow,
                .search = searchWidth,
                .all = allWidth,
                .info = infoWidth,
                .warning = warningWidth,
                .error = errorWidth,
                .source = sourceWidth,
                .collapse = actionWidth,
                .autoScroll = autoWidth,
                .clear = clearWidth};
    }

    void GlobalDockConsolePane::DrawToolbarSearch(float &x, const float y, const ToolbarLayout &toolbar, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        const auto &fonts = context.theme.fonts;
        ImGui::SetCursorScreenPos({x, y});
        if (Ui::InputTextControl("##ConsoleSearch", m_search.data(), m_search.size(), fonts,
                                 {.width = toolbar.search / l.scale,
                                  .hint = context.localization.Get("editor", "workspace.global_dock.console.search").c_str(),
                                  .prefixIconWidth = 18.0F,
                                  .surface = Ui::InputTextSurface::BottomDockToolbar}))
            m_filterDirty = true;
        Ui::DrawEditorIcon(ImGui::GetWindowDrawList(), Ui::UiIcon::Search, {x + l.gap, y + (l.control - l.icon) * 0.5F}, {l.icon, l.icon},
                           Theme::U32(Theme::Dim()), fonts.icon);
        x += toolbar.search + l.gap;
    }

    void GlobalDockConsolePane::DrawToolbarFilters(float &x, const float y, const ToolbarLayout &toolbar, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        const auto &fonts = context.theme.fonts;
        const auto filter = [&](const float buttonWidth, const LevelFilter value, const ToolbarButtonProps &props) {
            if (ToolbarButton({x, y}, {buttonWidth, l.control}, props, fonts)) {
                m_levelFilter = value;
                m_filterDirty = true;
            }
            x += buttonWidth + l.gap;
        };
        const auto label = [&](const char *key) {
            return toolbar.narrow ? nullptr : context.localization.Get("editor", key).c_str();
        };
        filter(toolbar.all, LevelFilter::All,
               {"##All", Ui::UiIcon::None, label("workspace.global_dock.console.filter.all"), m_matchingRecordCount, Theme::Muted(),
                m_levelFilter == LevelFilter::All});
        filter(toolbar.info, LevelFilter::Info,
               {"##Info", Ui::UiIcon::Info, label("workspace.global_dock.console.filter.info"), m_visibleLevelCounts[0],
                Theme::ConsoleInfo(), m_levelFilter == LevelFilter::Info});
        filter(toolbar.warning, LevelFilter::Warning,
               {"##Warning", Ui::UiIcon::Warning, label("workspace.global_dock.console.filter.warn"), m_visibleLevelCounts[1],
                Theme::ConsoleWarning(), m_levelFilter == LevelFilter::Warning});
        filter(toolbar.error, LevelFilter::Error,
               {"##Error", Ui::UiIcon::Error, label("workspace.global_dock.console.filter.error"), m_visibleLevelCounts[2],
                Theme::ConsoleError(), m_levelFilter == LevelFilter::Error});
    }

    void GlobalDockConsolePane::DrawToolbarSource(float &x, const float y, const ToolbarLayout &toolbar, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        ImGui::SetCursorScreenPos({x, y});
        const std::string &allSources = context.localization.Get("editor", "workspace.global_dock.console.source.all");
        std::vector<const char *> sourceItems;
        sourceItems.reserve(m_sources.size() + 1U);
        sourceItems.push_back(allSources.c_str());
        int selectedSourceIndex = 0;
        for (std::size_t index = 0; index < m_sources.size(); ++index) {
            sourceItems.push_back(m_sources[index].c_str());
            if (m_sources[index] == m_selectedSource)
                selectedSourceIndex = static_cast<int>(index + 1U);
        }
        ImGui::SetNextItemWidth(toolbar.source);
        if (Ui::ComboControl("ConsoleSource", &selectedSourceIndex, sourceItems.data(), static_cast<int>(sourceItems.size()), fonts,
                             Ui::ComboControlOptions{.height = 30.0F,
                                                     .componentSize = Ui::ComponentSize::Medium,
                                                     .surface = Ui::ComboControlSurface::BottomDockToolbar})) {
            m_selectedSource = selectedSourceIndex == 0 ? std::string{} : m_sources[static_cast<std::size_t>(selectedSourceIndex - 1)];
            m_filterDirty = true;
        }
        x += toolbar.source + GetLayout().gap;
    }

    void GlobalDockConsolePane::DrawToolbarActions(float x, const float y, const ToolbarLayout &toolbar, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        const auto &fonts = context.theme.fonts;
        const auto label = [&](const char *key) {
            return toolbar.narrow ? nullptr : context.localization.Get("editor", key).c_str();
        };
        if (ToolbarButton({x, y}, {toolbar.collapse, l.control},
                          {"##Collapse", Ui::UiIcon::ViewList, label("workspace.global_dock.console.collapse"), std::nullopt,
                           Theme::Muted(), m_compactRows},
                          fonts))
            m_compactRows = !m_compactRows;
        x += toolbar.collapse + l.gap;
        if (ToolbarButton({x, y}, {toolbar.autoScroll, l.control},
                          {"##AutoScroll", m_autoScroll ? Ui::UiIcon::Check : Ui::UiIcon::CheckboxUnchecked,
                           label("workspace.global_dock.console.auto_scroll"), std::nullopt,
                           m_autoScroll ? Theme::Accent() : Theme::Muted(), m_autoScroll},
                          fonts))
            m_autoScroll = !m_autoScroll;
        x += toolbar.autoScroll + l.gap;
        if (ToolbarButton({x, y}, {toolbar.clear, l.control},
                          {"##Clear", Ui::UiIcon::Delete, label("workspace.global_dock.console.clear"), std::nullopt, Theme::Muted(),
                           false},
                          fonts)) {
            if (!m_snapshot.records.empty())
                m_clearedThroughSequence = m_snapshot.records.back()->sequence;
            m_filterDirty = true;
            m_selectedSequence.reset();
        }
    }

    void GlobalDockConsolePane::DrawTableHeader(const ImVec2 &minimum, const float width, const EditorGuiContext &context) const {
        const Layout l = GetLayout();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        DrawGlobalDockTableHeaderSurface(minimum, width, l.header);
        const float size = Theme::TextPx::Caption();
        const float y = minimum.y + (l.header - size) * 0.5F;
        float x = minimum.x + l.padding;
        const auto header = [&](const char *key, const float column) {
            const std::string &label = context.localization.Get("editor", key);
            ClippedText(*drawList, context.theme.fonts.sans, size, {x, y}, {x + column - l.columnGap, minimum.y + l.header}, Theme::Muted(),
                        label);
            x += column + l.columnGap;
        };
        header("workspace.global_dock.console.column.time", l.time);
        header("workspace.global_dock.console.column.level", l.level);
        header("workspace.global_dock.console.column.source", l.source);
        header("workspace.global_dock.console.column.message", std::max(0.0F, minimum.x + width - x - l.padding));
    }

    void GlobalDockConsolePane::DrawLogRows(const float width, const float height, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        const float rowHeight = m_compactRows ? l.compactRow : l.row;
        const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
        if (m_filteredIndices.empty()) {
            DrawEmptyLogState(height, context);
            return;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredIndices.size()), rowHeight);
        while (clipper.Step()) {
            for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
                const auto &record = *m_snapshot.records[m_filteredIndices[static_cast<std::size_t>(visible)]];
                const ImVec2 rowMin{contentOrigin.x, contentOrigin.y + rowHeight * static_cast<float>(visible)};
                DrawLogRow(rowMin, width, rowHeight, record, context);
            }
        }
    }

    void GlobalDockConsolePane::DrawEmptyLogState(const float height, const EditorGuiContext &context) const {
        const float fontSize = Theme::TextPx::Caption();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const std::string &empty = context.localization.Get("editor", "workspace.global_dock.console.empty");
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float textWidth = TextWidth(context.theme.fonts.sans, fontSize, empty.c_str());
        ImGui::GetWindowDrawList()->AddText(context.theme.fonts.sans, fontSize,
                                            {origin.x + std::max(0.0F, (availableWidth - textWidth) * 0.5F),
                                             origin.y + std::max(0.0F, (height - fontSize) * 0.5F)},
                                            Theme::U32(Theme::Dim()), empty.c_str());
    }

    void GlobalDockConsolePane::DrawLogRow(const ImVec2 &origin, const float width, const float height,
                                           const Log::StructuredLogRecord &record, const EditorGuiContext &context) {
        const Layout l = GetLayout();
        const Severity severity = GetSeverity(record.level);
        const ImVec2 maximum{origin.x + width, origin.y + height};
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushID(&record);
        const bool selected = m_selectedSequence == record.sequence;
        if (ImGui::InvisibleButton("##Row", {width, height}))
            m_selectedSequence = selected ? std::nullopt : std::optional{record.sequence};
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (severity == Severity::Error)
            drawList->AddRectFilled(origin, maximum, Theme::U32(Theme::ConsoleErrorSurface()));
        if (selected || hovered)
            drawList->AddRectFilled(origin, maximum, Theme::U32(selected ? Theme::AccentSoft() : Theme::Hover()));
        if (severity == Severity::Error)
            drawList->AddRect(origin, maximum, Theme::U32(Theme::ConsoleErrorBorder()));
        drawList->AddLine({origin.x, maximum.y}, maximum, Theme::U32(Theme::ConsoleRowBorder()));
        const float fontSize = Theme::TextPx::Caption();
        const float y = origin.y + (height - fontSize) * 0.5F;
        float x = origin.x + l.padding;
        const std::string time = TimeLabel(record.timestampUtc);
        ClippedText(*drawList, context.theme.fonts.sans, fontSize, {x, y}, {x + l.time, maximum.y}, Theme::Dim(), time);
        x += l.time + l.columnGap;
        Ui::DrawEditorIcon(drawList, SeverityIcon(severity), {x, origin.y + (height - l.icon) * 0.5F}, {l.icon, l.icon},
                           Theme::U32(SeverityColor(severity)), context.theme.fonts.icon);
        ClippedText(*drawList, context.theme.fonts.sans, fontSize, {x + l.icon + l.gap, y}, {x + l.level, maximum.y},
                    SeverityColor(severity), context.localization.Get("editor", SeverityLabelKey(severity)));
        x += l.level + l.columnGap;
        const std::string source = SourceLabel(record);
        ClippedText(*drawList, context.theme.fonts.sans, fontSize, {x, y}, {x + l.source, maximum.y}, Theme::ConsoleSourceText(), source);
        x += l.source + l.columnGap;
        ClippedText(*drawList, context.theme.fonts.sans, fontSize, {x, y}, {maximum.x - l.padding, maximum.y}, Theme::ConsoleMessageText(),
                    record.message);
    }

    bool GlobalDockConsolePane::RefreshSnapshot() {
        if (m_logQuery == nullptr)
            return false;
        auto changed = m_logQuery->SnapshotIfChanged(m_revision);
        if (!changed.has_value())
            return false;
        m_snapshot = std::move(*changed);
        m_revision = m_snapshot.revision;
        m_filterDirty = true;
        return true;
    }

    bool GlobalDockConsolePane::MatchesSearch(const Log::StructuredLogRecord &record, const std::string &source) const {
        if (!m_selectedSource.empty() && source != m_selectedSource)
            return false;
        const std::string_view search{m_search.data()};
        return ContainsInsensitive(record.category, search) || ContainsInsensitive(record.message, search) ||
               ContainsInsensitive(record.context, search) || ContainsInsensitive(source, search);
    }

    bool GlobalDockConsolePane::MatchesLevel(const Log::StructuredLogRecord &record) const noexcept {
        const Severity severity = GetSeverity(record.level);
        if (m_levelFilter == LevelFilter::All)
            return true;
        if (m_levelFilter == LevelFilter::Info)
            return severity == Severity::Info;
        if (m_levelFilter == LevelFilter::Warning)
            return severity == Severity::Warning;
        return severity == Severity::Error;
    }

    void GlobalDockConsolePane::RebuildFilter() {
        m_filteredIndices.clear();
        m_sources.clear();
        m_matchingRecordCount = 0U;
        m_visibleLevelCounts.fill(0U);
        for (std::size_t index = 0; index < m_snapshot.records.size(); ++index) {
            const auto &record = *m_snapshot.records[index];
            if (m_clearedThroughSequence.has_value() && record.sequence <= *m_clearedThroughSequence)
                continue;
            const std::string source = SourceLabel(record);
            if (std::ranges::find(m_sources, source) == m_sources.end())
                m_sources.push_back(source);
            if (!MatchesSearch(record, source))
                continue;
            const Severity severity = GetSeverity(record.level);
            ++m_matchingRecordCount;
            ++m_visibleLevelCounts[static_cast<std::size_t>(severity)];
            if (MatchesLevel(record))
                m_filteredIndices.push_back(index);
        }
        std::ranges::sort(m_sources);
        m_filterDirty = false;
    }
}  // namespace Horo::Editor
