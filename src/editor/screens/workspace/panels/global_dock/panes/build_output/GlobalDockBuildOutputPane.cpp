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

        /** @brief Shows full record text and source without changing row activation. */
        void DrawRecordTooltip(const BuildOutputRecord &record) {
            if (!ImGui::IsItemHovered() || (!record.source.has_value() && record.message.empty()))
                return;
            ImGui::BeginTooltip();
            if (!record.message.empty())
                ImGui::TextUnformatted(record.message.c_str());
            if (record.source.has_value()) {
                const std::string fullSource = FormatSource(record.source);
                ImGui::TextUnformatted(fullSource.c_str());
            }
            ImGui::EndTooltip();
        }

    }  // namespace

    /** @copydoc GlobalDockBuildOutputPane::ProjectStatusPresentation */
    GlobalDockBuildOutputPane::BuildStatusPresentation GlobalDockBuildOutputPane::ProjectStatusPresentation(
        const BuildOutputRecord &record) noexcept {
        return PresentedBuildStatuses[static_cast<std::size_t>(PresentedStatus(record))];
    }

    /** @copydoc GlobalDockBuildOutputPane::ResolveColumns */
    GlobalDockBuildOutputPane::ColumnLayout GlobalDockBuildOutputPane::ResolveColumns(const GlobalDockPaneRegions &regions,
                                                                                      const GlobalDockPaneMetrics &metrics,
                                                                                      const float scale) noexcept {
        const float left = regions.contentOrigin.x + metrics.contentPadding;
        const float right = regions.contentOrigin.x + regions.contentWidth - metrics.contentPadding - 32.0F * scale;
        const float available = std::max(0.0F, right - left);
        const bool showLine = available >= 520.0F * scale;
        const bool showFile = available >= 360.0F * scale;
        const float levelWidth = std::min(84.0F * scale, available * 0.38F);
        const float line = left + levelWidth + metrics.columnGap;
        const float file = line + (showLine ? 70.0F * scale + metrics.columnGap : 0.0F);
        const float message = file + (showFile ? std::min(132.0F * scale, available * 0.22F) + metrics.columnGap : 0.0F);
        return {.level = left,
                .line = line,
                .file = file,
                .message = message,
                .right = right,
                .action = right + 5.0F * scale,
                .showLine = showLine,
                .showFile = showFile};
    }

    /** @copydoc GlobalDockBuildOutputPane::Attach */
    void GlobalDockBuildOutputPane::Attach(const IBuildOutputQuery *buildOutputQuery,
                                           const Application::GameplayBuildService *gameplayBuilds, const std::string_view projectRoot) {
        m_buildOutputQuery = buildOutputQuery;
        m_gameplayBuilds = gameplayBuilds;
        m_projectRoot = projectRoot;
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
        m_search.fill('\0');
        m_statusFilter = StatusFilter::All;
        m_severityFilter.reset();
        m_sessionFilter.reset();
        m_stageFilter.clear();
        m_clearBeforeSequence = 0;
        m_selectedSequence.reset();
        m_filterDirty = true;
    }

    /** @copydoc GlobalDockBuildOutputPane::Detach */
    void GlobalDockBuildOutputPane::Detach() noexcept {
        m_buildOutputQuery = nullptr;
        m_gameplayBuilds = nullptr;
        m_projectRoot.clear();
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
        m_severityFilter.reset();
        m_sessionFilter.reset();
        m_stageFilter.clear();
        m_clearBeforeSequence = 0;
        m_selectedSequence.reset();
    }

    /** @copydoc GlobalDockBuildOutputPane::Draw */
    void GlobalDockBuildOutputPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, EditorWorkspaceViewCommandData &command,
                                         const EditorGuiContext &context) {
        const bool snapshotChanged = RefreshSnapshot();
        if (m_filterDirty)
            RebuildFilter();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true, .hasFooter = true});
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const auto active =
            m_gameplayBuilds != nullptr && !m_projectRoot.empty() ? m_gameplayBuilds->QueryActiveProject(m_projectRoot) : std::nullopt;

        std::size_t errorCount = 0U;
        std::size_t warningCount = 0U;
        for (const BuildOutputRecord &record : m_snapshot.records) {
            errorCount += IsErrorRecord(record) ? 1U : 0U;
            warningCount += IsWarningRecord(record) ? 1U : 0U;
        }
        DrawToolbar(regions, metrics, context, errorCount, warningCount);
        if (active.has_value()) {
            const float activeHeight = std::min(metrics.toolbarHeight, std::max(0.0F, regions.contentHeight - metrics.tableHeaderHeight));
            if (activeHeight >= metrics.controlHeight) {
                DrawActiveBuild(*active, regions, metrics, context, activeHeight);
                regions.contentOrigin.y += activeHeight;
                regions.contentHeight -= activeHeight;
            }
        }
        DrawTable(regions, metrics, context, command, snapshotChanged);
        std::size_t visibleErrors = 0U;
        std::size_t visibleWarnings = 0U;
        for (const std::size_t index : m_filteredIndices) {
            visibleErrors += IsErrorRecord(m_snapshot.records[index]) ? 1U : 0U;
            visibleWarnings += IsWarningRecord(m_snapshot.records[index]) ? 1U : 0U;
        }
        DrawFooter(regions, metrics, context, visibleErrors, visibleWarnings);
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
        const ColumnLayout columns = ResolveColumns(regions, metrics, scale);
        const auto draw = [&](const float x, const float maximum, const char *key) {
            DrawGlobalDockClippedText(*drawList, context.theme.fonts.sansCompact, Theme::TextPx::Caption(), {x, headerY},
                                      {maximum, headerMin.y + metrics.tableHeaderHeight}, Theme::Muted(),
                                      context.localization.Get("editor", key));
        };
        draw(columns.level, columns.line - metrics.columnGap, "workspace.global_dock.build_output.column.level");
        if (columns.showLine)
            draw(columns.line, columns.file - metrics.columnGap, "workspace.global_dock.build_output.column.line");
        if (columns.showFile)
            draw(columns.file, columns.message - metrics.columnGap, "workspace.global_dock.build_output.column.file");
        draw(columns.message, columns.right, "workspace.global_dock.build_output.column.message");
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
        if (snapshotChanged && wasAtBottom)
            ImGui::SetScrollHereY(1.0F);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockBuildOutputPane::DrawTableRow(const BuildOutputRecord &record, const std::size_t visibleIndex,
                                                 const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                 const EditorGuiContext &context, EditorWorkspaceViewCommandData &command,
                                                 ImDrawList &drawList) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const ColumnLayout columns = ResolveColumns(regions, metrics, scale);
        const ImVec2 rowMin{regions.contentOrigin.x, regions.contentOrigin.y + metrics.tableHeaderHeight +
                                                         static_cast<float>(visibleIndex) * metrics.tableRowHeight - ImGui::GetScrollY()};
        ImGui::SetCursorScreenPos(rowMin);
        ImGui::PushID(static_cast<int>(visibleIndex));
        const bool activated = ImGui::InvisibleButton("##diagnostic", {regions.contentWidth, metrics.tableRowHeight});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (activated)
            m_selectedSequence = record.sequence;
        DrawGlobalDockTableRowSurface(rowMin, regions.contentWidth, metrics.tableRowHeight, hovered, m_selectedSequence == record.sequence);
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        const std::string level = context.localization.Get("editor", StatusLocalizationKey(record));
        const std::string line = LineLabel(record);
        const std::string file = FileLabel(record);
        DrawGlobalDockEllipsizedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {columns.level, textY},
                                     {columns.line - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, StatusColor(record), level);
        if (columns.showLine)
            DrawGlobalDockEllipsizedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {columns.line, textY},
                                         {columns.file - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(), line);
        if (columns.showFile)
            DrawGlobalDockEllipsizedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {columns.file, textY},
                                         {columns.message - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Accent(), file);
        DrawGlobalDockEllipsizedText(drawList, context.theme.fonts.sansCompact, Theme::TextPx::Label(), {columns.message, textY},
                                     {columns.right, rowMin.y + metrics.tableRowHeight}, Theme::Text(), record.message);
        if (record.source.has_value()) {
            const float iconSize = 14.0F * scale;
            Ui::DrawEditorIcon(&drawList, Ui::UiIcon::ArrowForward, {columns.action, rowMin.y + (metrics.tableRowHeight - iconSize) * 0.5F},
                               {iconSize, iconSize}, Theme::U32(Theme::Accent()), context.theme.fonts.icon);
        }
        DrawRecordTooltip(record);
        if (activated && record.source.has_value()) {
            command.command = EditorWorkspaceViewCommand::OpenDiagnosticSource;
            command.diagnosticSource = DiagnosticSourceRequest{
                .absolutePath = record.source->absolutePath,
                .line = record.source->line,
                .column = record.source->column,
            };
        }
    }

    void GlobalDockBuildOutputPane::DrawFooter(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                               const EditorGuiContext &context, const std::size_t errorCount,
                                               const std::size_t warningCount) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const std::string summary =
            std::format("{} {}   {} {}   {} {}", m_filteredIndices.size(),
                        context.localization.Get("editor", "workspace.global_dock.build_output.footer.diagnostics"), errorCount,
                        context.localization.Get("editor", "workspace.global_dock.build_output.footer.errors"), warningCount,
                        context.localization.Get("editor", "workspace.global_dock.build_output.footer.warnings"));
        const std::string dropped =
            m_snapshot.droppedRecordCount == 0U
                ? std::string{}
                : std::format("   {} {}", m_snapshot.droppedRecordCount,
                              context.localization.Get("editor", "workspace.global_dock.build_output.footer.dropped"));
        const std::string leftText = summary + dropped;
        DrawGlobalDockFooterSurface(regions.footerOrigin, regions.footerWidth, metrics.footerHeight);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float footerY = regions.footerOrigin.y + (metrics.footerHeight - Theme::TextPx::Caption()) * 0.5F;
        DrawGlobalDockEllipsizedText(*drawList, fonts.sansCompact, Theme::TextPx::Caption(),
                                     {regions.footerOrigin.x + metrics.contentPadding, footerY},
                                     {regions.footerOrigin.x + regions.footerWidth - metrics.contentPadding,
                                      regions.footerOrigin.y + metrics.footerHeight},
                                     Theme::Muted(), leftText);
        if (m_snapshot.records.empty())
            return;
        const BuildOutputRecord &last = m_snapshot.records.back();
        const std::string lastBuild =
            std::format("{} {} · {}", context.localization.Get("editor", "workspace.global_dock.build_output.footer.last_build"),
                        FormatTimeOfDay(last.timestampUtc), context.localization.Get("editor", StatusLocalizationKey(last)));
        const float textWidth = (fonts.sansCompact != nullptr ? fonts.sansCompact : ImGui::GetFont())
                                    ->CalcTextSizeA(Theme::TextPx::Caption(), FLT_MAX, 0.0F, lastBuild.c_str())
                                    .x;
        const float summaryWidth = MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Caption(), leftText);
        if (summaryWidth > regions.footerWidth - metrics.contentPadding * 2.0F &&
            ImGui::IsMouseHoveringRect(regions.footerOrigin,
                                       {regions.footerOrigin.x + regions.footerWidth, regions.footerOrigin.y + metrics.footerHeight})) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(leftText.c_str());
            ImGui::EndTooltip();
        }
        if (summaryWidth + textWidth + metrics.contentPadding * 3.0F > regions.footerWidth)
            return;
        drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(),
                          {regions.footerOrigin.x + regions.footerWidth - metrics.contentPadding - textWidth, footerY},
                          Theme::U32(Theme::Muted()), lastBuild.c_str());
    }

    bool GlobalDockBuildOutputPane::RefreshSnapshot() {
        if (m_buildOutputQuery == nullptr)
            return false;
        auto changed = m_buildOutputQuery->SnapshotIfChanged(m_revision);
        if (!changed.has_value())
            return false;
        m_snapshot = std::move(*changed);
        if (m_selectedSequence.has_value() && std::ranges::none_of(m_snapshot.records, [&](const BuildOutputRecord &record) {
            return record.sequence == *m_selectedSequence;
        }))
            m_selectedSequence.reset();
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
        return ProjectRecords(records, RecordFilter{.status = statusFilter, .search = search});
    }

    /** @copydoc GlobalDockBuildOutputPane::ProjectRecords */
    std::vector<std::size_t> GlobalDockBuildOutputPane::ProjectRecords(const std::span<const BuildOutputRecord> records,
                                                                       const RecordFilter &filter) {
        std::vector<std::size_t> projected;
        projected.reserve(records.size());
        for (std::size_t index = 0; index < records.size(); ++index) {
            const BuildOutputRecord &record = records[index];
            if (record.sequence != 0U && record.sequence <= filter.afterSequence)
                continue;
            if (!PassesStatusFilter(record, filter.status))
                continue;
            if (filter.severity.has_value() && record.severity != *filter.severity)
                continue;
            if (filter.session.has_value() && record.sessionId != filter.session)
                continue;
            if (!filter.stage.empty() && record.stage != filter.stage)
                continue;
            if (!filter.search.empty() && !GlobalDockContainsCaseInsensitive(record.stage, filter.search) &&
                !GlobalDockContainsCaseInsensitive(record.code.Value(), filter.search) &&
                !GlobalDockContainsCaseInsensitive(record.message, filter.search) &&
                !GlobalDockContainsCaseInsensitive(FormatSource(record.source), filter.search) &&
                !GlobalDockContainsCaseInsensitive(TechnicalStatusText(record), filter.search))
                continue;
            projected.push_back(index);
        }
        return projected;
    }

    void GlobalDockBuildOutputPane::RebuildFilter() {
        m_filteredIndices = ProjectRecords(m_snapshot.records, RecordFilter{.status = m_statusFilter,
                                                                            .severity = m_severityFilter,
                                                                            .session = m_sessionFilter,
                                                                            .stage = m_stageFilter,
                                                                            .search = std::string_view{m_search.data()},
                                                                            .afterSequence = m_clearBeforeSequence});
        m_filterDirty = false;
    }
}  // namespace Horo::Editor
