#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        enum class PresentedBuildStatus : std::uint8_t {
            Succeeded,
            Failed,
            Cached,
            Cancelled,
            TimedOut,
            Fatal,
            Error,
            Warning,
            Info,
            Unknown,
            UseSeverity,
        };

        using BuildStatusColorRole = GlobalDockBuildOutputPane::BuildStatusColorRole;
        using BuildStatusPresentation = GlobalDockBuildOutputPane::BuildStatusPresentation;

        constexpr std::array PresentedBuildStatuses{
            BuildStatusPresentation{BuildStatusColorRole::Positive, "OK", "workspace.global_dock.build_output.row_status.succeeded"},
            BuildStatusPresentation{BuildStatusColorRole::Error, "FAILED", "workspace.global_dock.build_output.row_status.failed"},
            BuildStatusPresentation{BuildStatusColorRole::Muted, "CACHED", "workspace.global_dock.build_output.row_status.cached"},
            BuildStatusPresentation{BuildStatusColorRole::Warning, "CANCELLED", "workspace.global_dock.build_output.row_status.cancelled"},
            BuildStatusPresentation{BuildStatusColorRole::Error, "TIMED OUT", "workspace.global_dock.build_output.row_status.timed_out"},
            BuildStatusPresentation{BuildStatusColorRole::Error, "FATAL", "workspace.global_dock.build_output.row_status.fatal"},
            BuildStatusPresentation{BuildStatusColorRole::Error, "ERROR", "workspace.global_dock.build_output.row_status.failed"},
            BuildStatusPresentation{BuildStatusColorRole::Warning, "WARNING", "workspace.global_dock.build_output.row_status.warning"},
            BuildStatusPresentation{BuildStatusColorRole::Accent, "INFO", "workspace.global_dock.build_output.row_status.info"},
            BuildStatusPresentation{BuildStatusColorRole::Default, "INFO", "workspace.global_dock.build_output.row_status.info"},
        };
        static_assert(PresentedBuildStatuses.size() == static_cast<std::size_t>(PresentedBuildStatus::UseSeverity));

        using BuildStatusColorResolver = ImVec4 (*)();
        constexpr std::array BuildStatusColorResolvers{Theme::Ok, Theme::Err, Theme::Dim, Theme::Warn, Theme::Accent, Theme::Text};
        static_assert(BuildStatusColorResolvers.size() == static_cast<std::size_t>(BuildStatusColorRole::Count));

        [[nodiscard]] constexpr PresentedBuildStatus ResultStatus(const BuildOutputResult result) noexcept {
            using enum BuildOutputResult;
            switch (result) {
                case Succeeded:
                    return PresentedBuildStatus::Succeeded;
                case Failed:
                    return PresentedBuildStatus::Failed;
                case Cached:
                    return PresentedBuildStatus::Cached;
                case Cancelled:
                    return PresentedBuildStatus::Cancelled;
                case TimedOut:
                    return PresentedBuildStatus::TimedOut;
                case None:
                    return PresentedBuildStatus::UseSeverity;
            }
            return PresentedBuildStatus::UseSeverity;
        }

        [[nodiscard]] constexpr PresentedBuildStatus SeverityStatus(const DiagnosticSeverity severity) noexcept {
            using enum DiagnosticSeverity;
            switch (severity) {
                case Fatal:
                    return PresentedBuildStatus::Fatal;
                case Error:
                    return PresentedBuildStatus::Error;
                case Warning:
                    return PresentedBuildStatus::Warning;
                case Note:
                    return PresentedBuildStatus::Info;
            }
            return PresentedBuildStatus::Unknown;
        }

        [[nodiscard]] constexpr PresentedBuildStatus PresentedStatus(const BuildOutputRecord &record) noexcept {
            const PresentedBuildStatus result = ResultStatus(record.result);
            return result == PresentedBuildStatus::UseSeverity ? SeverityStatus(record.severity) : result;
        }

        [[nodiscard]] ImVec4 StatusColor(const BuildOutputRecord &record) noexcept {
            const auto presentation = GlobalDockBuildOutputPane::ProjectStatusPresentation(record);
            return BuildStatusColorResolvers[static_cast<std::size_t>(presentation.colorRole)]();
        }

        [[nodiscard]] const char *TechnicalStatusText(const BuildOutputRecord &record) noexcept {
            return GlobalDockBuildOutputPane::ProjectStatusPresentation(record).technicalText.data();
        }

        [[nodiscard]] const char *StatusLocalizationKey(const BuildOutputRecord &record) noexcept {
            return GlobalDockBuildOutputPane::ProjectStatusPresentation(record).localizationKey.data();
        }

        [[nodiscard]] bool IsOkRecord(const BuildOutputRecord &record) noexcept {
            return record.result == BuildOutputResult::Succeeded ||
                   (record.result == BuildOutputResult::None && record.severity == DiagnosticSeverity::Note);
        }

        [[nodiscard]] bool IsFailedRecord(const BuildOutputRecord &record) noexcept {
            using enum BuildOutputResult;
            using enum DiagnosticSeverity;
            return record.result == Failed || record.result == Cancelled || record.result == TimedOut ||
                   (record.result == None && (record.severity == Error || record.severity == Fatal));
        }

        [[nodiscard]] bool IsWarningRecord(const BuildOutputRecord &record) noexcept {
            return record.result == BuildOutputResult::Cancelled ||
                   (record.result == BuildOutputResult::None && record.severity == DiagnosticSeverity::Warning);
        }

        [[nodiscard]] bool IsErrorRecord(const BuildOutputRecord &record) noexcept {
            using enum BuildOutputResult;
            using enum DiagnosticSeverity;
            return record.result == Failed || record.result == TimedOut ||
                   (record.result == None && (record.severity == Error || record.severity == Fatal));
        }

        [[nodiscard]] std::string LineLabel(const BuildOutputRecord &record) {
            if (!record.source.has_value() || record.source->line == 0U)
                return "—";
            if (record.source->column == 0U)
                return std::to_string(record.source->line);
            return std::format("{}:{}", record.source->line, record.source->column);
        }

        [[nodiscard]] std::string FileLabel(const BuildOutputRecord &record) {
            if (!record.source.has_value() || record.source->absolutePath.empty())
                return record.stage.empty() ? "Editor" : record.stage;
            return std::filesystem::path{record.source->absolutePath}.filename().string();
        }

        [[nodiscard]] std::string FormatTimeOfDay(const std::chrono::system_clock::time_point &timestampUtc) {
            const std::time_t timeT = std::chrono::system_clock::to_time_t(timestampUtc);
            std::tm tmValue{};
#if defined(_WIN32)
            gmtime_s(&tmValue, &timeT);
#else
            gmtime_r(&timeT, &tmValue);
#endif
            return std::format("{:02d}:{:02d}:{:02d}", tmValue.tm_hour, tmValue.tm_min, tmValue.tm_sec);
        }

        [[nodiscard]] std::string FormatSource(const std::optional<DiagnosticSourceLocation> &source) {
            if (!source.has_value())
                return {};
            if (source->line == 0U)
                return source->absolutePath;
            if (source->column == 0U)
                return std::format("{}:{}", source->absolutePath, source->line);
            return std::format("{}:{}:{}", source->absolutePath, source->line, source->column);
        }
    }  // namespace

    /** @copydoc GlobalDockBuildOutputPane::ProjectStatusPresentation */
    GlobalDockBuildOutputPane::BuildStatusPresentation GlobalDockBuildOutputPane::ProjectStatusPresentation(
        const BuildOutputRecord &record) noexcept {
        return PresentedBuildStatuses[static_cast<std::size_t>(PresentedStatus(record))];
    }

    /** @copydoc GlobalDockBuildOutputPane::Attach */
    void GlobalDockBuildOutputPane::Attach(const IBuildOutputQuery *buildOutputQuery) noexcept {
        m_buildOutputQuery = buildOutputQuery;
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
        m_filterDirty = true;
        m_initialFollowTail = true;
    }

    /** @copydoc GlobalDockBuildOutputPane::Detach */
    void GlobalDockBuildOutputPane::Detach() noexcept {
        m_buildOutputQuery = nullptr;
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
    }

    /** @copydoc GlobalDockBuildOutputPane::Draw */
    void GlobalDockBuildOutputPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, EditorWorkspaceViewCommandData &command,
                                         const EditorGuiContext &context) {
        const bool snapshotChanged = RefreshSnapshot();
        if (m_filterDirty)
            RebuildFilter();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();

        std::size_t errorCount = 0U;
        std::size_t warningCount = 0U;
        for (const BuildOutputRecord &record : m_snapshot.records) {
            errorCount += IsErrorRecord(record) ? 1U : 0U;
            warningCount += IsWarningRecord(record) ? 1U : 0U;
        }
        DrawToolbar(regions, metrics, context, errorCount, warningCount);
        DrawTable(regions, metrics, context, command, snapshotChanged);
    }

    void GlobalDockBuildOutputPane::DrawToolbar(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                const EditorGuiContext &context, const std::size_t errorCount,
                                                const std::size_t warningCount) {
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float x = DrawToolbarStatus(regions, metrics, context, errorCount, warningCount, controlY);
        DrawToolbarTargets(metrics, context, x, controlY);
    }

    float GlobalDockBuildOutputPane::DrawToolbarStatus(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                       const EditorGuiContext &context, const std::size_t errorCount,
                                                       const std::size_t warningCount, const float controlY) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        ToolbarStatusChipLayout layout = ResolveToolbarStatusChipLayout(context, errorCount, warningCount, scale, metrics.toolbarGap);
        const Theme::Fonts &fonts = context.theme.fonts;
        const float searchWidth = std::max(180.0F * scale, regions.toolbarWidth - metrics.toolbarPaddingX * 2.0F - layout.fixedWidth);
        float x = regions.toolbarOrigin.x + metrics.toolbarPaddingX;
        const std::string previousSearch{m_search.data()};
        x = DrawGlobalDockSearchControl({x, controlY}, searchWidth, "##BuildOutputSearch", m_search,
                                        context.localization.Get("editor", "workspace.global_dock.build_output.search"), fonts);
        if (previousSearch != std::string_view{m_search.data()})
            m_filterDirty = true;
        layout.x = x;
        layout.controlY = controlY;
        return DrawToolbarStatusChips(layout);
    }

    GlobalDockBuildOutputPane::ToolbarStatusChipLayout GlobalDockBuildOutputPane::ResolveToolbarStatusChipLayout(
        const EditorGuiContext &context, const std::size_t errorCount, const std::size_t warningCount, const float scale,
        const float gap) const {
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockToolbarChipProps allProps{
            .id = "BuildAll",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.status.all"),
            .count = m_snapshot.records.size(),
            .active = m_statusFilter == StatusFilter::All,
        };
        const GlobalDockToolbarChipProps errorProps{
            .id = "BuildErrors",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.status.failed"),
            .count = errorCount,
            .tone = GlobalDockTone::Error,
            .active = m_statusFilter == StatusFilter::Errors,
        };
        const GlobalDockToolbarChipProps warningProps{
            .id = "BuildWarnings",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.status.warning"),
            .count = warningCount,
            .tone = GlobalDockTone::Warning,
            .active = m_statusFilter == StatusFilter::Warning,
        };
        const GlobalDockToolbarChipProps rebuildProps{
            .id = "BuildRebuild",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.rebuild"),
            .tone = GlobalDockTone::Accent,
            .active = true,
            .icon = Ui::UiIcon::Reset,
        };
        const float allWidth = MeasureGlobalDockToolbarChip(allProps, fonts);
        const float errorWidth = MeasureGlobalDockToolbarChip(errorProps, fonts);
        const float warningWidth = MeasureGlobalDockToolbarChip(warningProps, fonts);
        const float rebuildWidth = MeasureGlobalDockToolbarChip(rebuildProps, fonts);
        return ToolbarStatusChipLayout{.x = 0.0F,
                                       .controlY = 0.0F,
                                       .scale = scale,
                                       .gap = gap,
                                       .context = &context,
                                       .allProps = allProps,
                                       .errorProps = errorProps,
                                       .warningProps = warningProps,
                                       .allWidth = allWidth,
                                       .errorWidth = errorWidth,
                                       .warningWidth = warningWidth,
                                       .fixedWidth = allWidth + errorWidth + warningWidth + rebuildWidth + (108.0F + 132.0F) * scale +
                                                     gap * 7.0F + scale};
    }

    float GlobalDockBuildOutputPane::DrawToolbarStatusChips(const ToolbarStatusChipLayout &layout) {
        const Theme::Fonts &fonts = layout.context->theme.fonts;
        float cursorX = layout.x;
        if (DrawGlobalDockToolbarChip({cursorX, layout.controlY}, layout.allWidth, layout.allProps, fonts)) {
            m_statusFilter = StatusFilter::All;
            m_filterDirty = true;
        }
        cursorX += layout.allWidth + layout.gap;
        if (DrawGlobalDockToolbarChip({cursorX, layout.controlY}, layout.errorWidth, layout.errorProps, fonts)) {
            m_statusFilter = StatusFilter::Errors;
            m_filterDirty = true;
        }
        cursorX += layout.errorWidth + layout.gap;
        if (DrawGlobalDockToolbarChip({cursorX, layout.controlY}, layout.warningWidth, layout.warningProps, fonts)) {
            m_statusFilter = StatusFilter::Warning;
            m_filterDirty = true;
        }
        cursorX += layout.warningWidth + layout.gap;
        DrawGlobalDockToolbarSeparator(cursorX, layout.controlY);
        return cursorX + layout.gap + layout.scale;
    }

    void GlobalDockBuildOutputPane::DrawToolbarTargets(const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context, float x,
                                                       const float controlY) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const float targetWidth = 108.0F * scale;
        const float configurationWidth = 132.0F * scale;
        const std::array<std::string, 3> targetText{
            context.localization.Get("editor", "workspace.global_dock.build_output.target.editor"),
            context.localization.Get("editor", "workspace.global_dock.build_output.target.runtime"),
            context.localization.Get("editor", "workspace.global_dock.build_output.target.tests"),
        };
        const std::array<const char *, 3> targetItems{targetText[0].c_str(), targetText[1].c_str(), targetText[2].c_str()};
        ImGui::SetCursorScreenPos({x, controlY});
        ImGui::SetNextItemWidth(targetWidth);
        static_cast<void>(Ui::ComboControl("BuildTarget", &m_targetSelection, targetItems.data(), static_cast<int>(targetItems.size()),
                                           fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += targetWidth + metrics.toolbarGap;

        const std::array<std::string, 3> configurationText{
            context.localization.Get("editor", "workspace.global_dock.build_output.configuration.development"),
            context.localization.Get("editor", "workspace.global_dock.build_output.configuration.debug"),
            context.localization.Get("editor", "workspace.global_dock.build_output.configuration.release"),
        };
        const std::array<const char *, 3> configurationItems{configurationText[0].c_str(), configurationText[1].c_str(),
                                                             configurationText[2].c_str()};
        ImGui::SetCursorScreenPos({x, controlY});
        ImGui::SetNextItemWidth(configurationWidth);
        static_cast<void>(Ui::ComboControl("BuildConfiguration", &m_configurationSelection, configurationItems.data(),
                                           static_cast<int>(configurationItems.size()), fonts,
                                           {.height = GlobalDockLayout::ControlHeight,
                                            .componentSize = Ui::ComponentSize::Small,
                                            .surface = Ui::ComboControlSurface::BottomDockToolbar}));
        x += configurationWidth + metrics.toolbarGap;
        const GlobalDockToolbarChipProps rebuildProps{
            .id = "BuildRebuild",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.rebuild"),
            .tone = GlobalDockTone::Accent,
            .active = true,
            .icon = Ui::UiIcon::Reset,
        };
        const float rebuildWidth = MeasureGlobalDockToolbarChip(rebuildProps, fonts);
        static_cast<void>(DrawGlobalDockToolbarChip({x, controlY}, rebuildWidth, rebuildProps, fonts));
    }

    void GlobalDockBuildOutputPane::DrawTable(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                              const EditorGuiContext &context, EditorWorkspaceViewCommandData &command,
                                              const bool snapshotChanged) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        DrawTableHeader(regions, metrics, context, scale);
        DrawTableRows(regions, metrics, context, command, snapshotChanged);
    }

    void GlobalDockBuildOutputPane::DrawTableHeader(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                    const EditorGuiContext &context, const float scale) const {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = regions.contentOrigin;
        DrawGlobalDockTableHeaderSurface(headerMin, regions.contentWidth, metrics.tableHeaderHeight);
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const std::array positions{
            headerMin.x + metrics.contentPadding,
            headerMin.x + metrics.contentPadding + 68.0F * scale + metrics.columnGap,
            headerMin.x + metrics.contentPadding + (68.0F + 74.0F) * scale + metrics.columnGap * 2.0F,
            headerMin.x + metrics.contentPadding + (68.0F + 74.0F + 96.0F) * scale + metrics.columnGap * 3.0F,
        };
        const std::array<const char *, 4> keys{
            "workspace.global_dock.build_output.column.level",
            "workspace.global_dock.build_output.column.line",
            "workspace.global_dock.build_output.column.file",
            "workspace.global_dock.build_output.column.message",
        };
        for (std::size_t index = 0; index < positions.size(); ++index)
            drawList->AddText(context.theme.fonts.sansCompact, Theme::TextPx::Caption(), {positions[index], headerY},
                              Theme::U32(Theme::Muted()), context.localization.Get("editor", keys[index]).c_str());
    }

    void GlobalDockBuildOutputPane::DrawTableRows(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                  const EditorGuiContext &context, EditorWorkspaceViewCommandData &command,
                                                  const bool snapshotChanged) {
        const float rowsHeight = std::max(1.0F, regions.contentHeight - metrics.tableHeaderHeight);
        const ImVec2 rowsOrigin{regions.contentOrigin.x, regions.contentOrigin.y + metrics.tableHeaderHeight};
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##BuildOutputRows", {regions.contentWidth, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const bool wasAtBottom = ImGui::GetScrollY() >= std::max(0.0F, ImGui::GetScrollMaxY() - 2.0F);
        for (std::size_t visibleIndex = 0; visibleIndex < m_filteredIndices.size(); ++visibleIndex)
            DrawTableRow(m_snapshot.records[m_filteredIndices[visibleIndex]], visibleIndex, regions, metrics, context, command, *drawList);
        if (snapshotChanged && (wasAtBottom || m_initialFollowTail))
            ImGui::SetScrollHereY(1.0F);
        m_initialFollowTail = false;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockBuildOutputPane::DrawTableRow(const BuildOutputRecord &record, const std::size_t visibleIndex,
                                                 const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                 const EditorGuiContext &context, EditorWorkspaceViewCommandData &command,
                                                 ImDrawList &drawList) const {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const float levelX = regions.contentOrigin.x + metrics.contentPadding;
        const float lineX = levelX + 68.0F * scale + metrics.columnGap;
        const float fileX = lineX + 74.0F * scale + metrics.columnGap;
        const float messageX = fileX + 96.0F * scale + metrics.columnGap;
        const ImVec2 rowMin{regions.contentOrigin.x, regions.contentOrigin.y + metrics.tableHeaderHeight +
                                                         static_cast<float>(visibleIndex) * metrics.tableRowHeight - ImGui::GetScrollY()};
        ImGui::SetCursorScreenPos(rowMin);
        ImGui::PushID(static_cast<int>(visibleIndex));
        const bool activated = ImGui::InvisibleButton("##diagnostic", {regions.contentWidth, metrics.tableRowHeight});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        DrawGlobalDockTableRowSurface(rowMin, regions.contentWidth, metrics.tableRowHeight, hovered);
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const std::string level = context.localization.Get("editor", StatusLocalizationKey(record));
        const std::string line = LineLabel(record);
        const std::string file = FileLabel(record);
        DrawGlobalDockClippedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {levelX, textY},
                                  {lineX - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, StatusColor(record), level);
        DrawGlobalDockClippedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {lineX, textY},
                                  {fileX - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(), line);
        DrawGlobalDockClippedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {fileX, textY},
                                  {messageX - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Accent(), file);
        DrawGlobalDockClippedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {messageX, textY},
                                  {rowMin.x + regions.contentWidth - metrics.contentPadding, rowMin.y + metrics.tableRowHeight},
                                  Theme::Text(), record.message);
        if (activated && record.source.has_value()) {
            command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
            command.diagnosticSource = DiagnosticSourceRequest{
                .absolutePath = record.source->absolutePath,
                .line = record.source->line,
                .column = record.source->column,
            };
        }
    }

    bool GlobalDockBuildOutputPane::RefreshSnapshot() {
        if (m_buildOutputQuery == nullptr)
            return false;
        auto changed = m_buildOutputQuery->SnapshotIfChanged(m_revision);
        if (!changed.has_value())
            return false;
        m_snapshot = std::move(*changed);
        m_revision = m_snapshot.revision;
        m_filterDirty = true;
        return true;
    }

    bool GlobalDockBuildOutputPane::PassesStatusFilter(const BuildOutputRecord &record, const StatusFilter filter) noexcept {
        switch (filter) {
            case StatusFilter::All:
                return true;
            case StatusFilter::Ok:
                return IsOkRecord(record);
            case StatusFilter::Failed:
                return IsFailedRecord(record);
            case StatusFilter::Cached:
                return record.result == BuildOutputResult::Cached;
            case StatusFilter::Warning:
                return IsWarningRecord(record);
            case StatusFilter::Errors:
                return IsErrorRecord(record);
        }
        return true;
    }

    std::vector<std::size_t> GlobalDockBuildOutputPane::ProjectRecords(const std::span<const BuildOutputRecord> records,
                                                                       const StatusFilter statusFilter, const std::string_view search) {
        std::vector<std::size_t> projected;
        projected.reserve(records.size());
        for (std::size_t index = 0; index < records.size(); ++index) {
            const BuildOutputRecord &record = records[index];
            const std::string source = FormatSource(record.source);
            if (!PassesStatusFilter(record, statusFilter))
                continue;
            if (!search.empty() && !GlobalDockContainsCaseInsensitive(record.stage, search) &&
                !GlobalDockContainsCaseInsensitive(record.code.Value(), search) &&
                !GlobalDockContainsCaseInsensitive(record.message, search) && !GlobalDockContainsCaseInsensitive(source, search) &&
                !GlobalDockContainsCaseInsensitive(TechnicalStatusText(record), search))
                continue;
            projected.push_back(index);
        }
        return projected;
    }

    void GlobalDockBuildOutputPane::RebuildFilter() {
        m_filteredIndices = ProjectRecords(m_snapshot.records, m_statusFilter, std::string_view{m_search.data()});
        m_filterDirty = false;
    }
}  // namespace Horo::Editor
