#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

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
        [[nodiscard]] const char *ActiveBuildStateKey(const Application::GameplayBuildState state) noexcept {
            using enum Application::GameplayBuildState;
            switch (state) {
                case Queued:
                    return "workspace.global_dock.build_output.active_state.queued";
                case AcquiringLock:
                    return "workspace.global_dock.build_output.active_state.acquiring_lock";
                case WaitingForExternalBuild:
                    return "workspace.global_dock.build_output.active_state.waiting";
                case Configuring:
                    return "workspace.global_dock.build_output.active_state.configuring";
                case Building:
                    return "workspace.global_dock.build_output.active_state.building";
                case Validating:
                    return "workspace.global_dock.build_output.active_state.validating";
                case Succeeded:
                case Failed:
                case Cancelled:
                case TimedOut:
                    return "workspace.global_dock.build_output.active_state.queued";
            }
            return "workspace.global_dock.build_output.active_state.queued";
        }

        void DrawFilterLabel(const EditorGuiContext &context, const char *key) {
            const std::string label = context.localization.Get("editor", key);
            Ui::FieldLabel(label.c_str(), context.theme.fonts);
        }

        bool DrawFilterCombo(const EditorGuiContext &context, const char *id, int &selection, const std::vector<std::string> &items) {
            std::vector<const char *> labels;
            labels.reserve(items.size());
            for (const std::string &item : items)
                labels.push_back(item.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            return Ui::ComboControl(id, &selection, labels.data(), static_cast<int>(labels.size()), context.theme.fonts);
        }
    }  // namespace

    void GlobalDockBuildOutputPane::DrawActiveBuild(const Application::GameplayBuildSnapshot &snapshot,
                                                    const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                    const EditorGuiContext &context, const float height) {
        const GlobalDockToolbarChipProps cancel{
            .id = "BuildCancelActive",
            .label = context.localization.Get("editor", snapshot.cancellationRequested ? "workspace.global_dock.build_output.cancelling"
                                                                                       : "workspace.global_dock.build_output.cancel"),
            .tone = GlobalDockTone::Warning,
            .disabled = snapshot.cancellationRequested,
        };
        const float y = regions.contentOrigin.y;
        const float buttonWidth = std::min(MeasureGlobalDockToolbarChip(cancel, context.theme.fonts),
                                           std::max(1.0F, regions.contentWidth - 2.0F * metrics.contentPadding));
        const float buttonX = regions.contentOrigin.x + regions.contentWidth - metrics.contentPadding - buttonWidth;
        const float controlY = y + (height - metrics.controlHeight) * 0.5F;
        DrawGlobalDockToolbarSurface({regions.contentOrigin.x, y}, regions.contentWidth, height);

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - snapshot.startedAt);
        const std::string progress =
            snapshot.progress.has_value() ? std::format(" · {}%", static_cast<int>(*snapshot.progress * 100.0F)) : std::string{};
        const std::string label =
            std::format("{} · {}{} · {}:{:02d}", context.localization.Get("editor", "workspace.global_dock.build_output.active"),
                        context.localization.Get("editor", ActiveBuildStateKey(snapshot.state)), progress, elapsed.count() / 60,
                        elapsed.count() % 60);
        const float textY = y + (height - Theme::TextPx::Label()) * 0.5F;
        DrawGlobalDockClippedText(*ImGui::GetWindowDrawList(), context.theme.fonts.sansCompact, Theme::TextPx::Label(),
                                  {regions.contentOrigin.x + metrics.contentPadding, textY}, {buttonX - metrics.toolbarGap, y + height},
                                  Theme::Text(), label);

        if (DrawGlobalDockToolbarChip({buttonX, controlY}, buttonWidth, cancel, context.theme.fonts))
            static_cast<void>(m_gameplayBuilds->RequestCancel(snapshot.id));
    }

    void GlobalDockBuildOutputPane::DrawToolbar(const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                const EditorGuiContext &context, const std::size_t errorCount,
                                                const std::size_t warningCount) {
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockToolbarChipProps filterProps{
            .id = "BuildFilters",
            .label = context.localization.Get("editor", "workspace.global_dock.build_output.filters"),
            .active = m_statusFilter != StatusFilter::All || m_severityFilter.has_value() || m_sessionFilter.has_value() ||
                      !m_stageFilter.empty(),
        };
        const float filterWidth = MeasureGlobalDockToolbarChip(filterProps, context.theme.fonts);
        ToolbarStatusChipLayout layout = ResolveToolbarStatusChipLayout(context, errorCount, warningCount, scale, metrics.toolbarGap);
        const bool showExtended =
            regions.toolbarWidth >= metrics.toolbarPaddingX * 2.0F + 140.0F * scale + layout.fixedWidth + filterWidth + metrics.toolbarGap;
        const float searchWidth = ResolveGlobalDockSearchWidth(regions.toolbarWidth, filterWidth + metrics.toolbarGap +
                                                                                         (showExtended ? layout.fixedWidth : 0.0F));
        const std::string previousSearch{m_search.data()};
        float x =
            DrawGlobalDockSearchControl({regions.toolbarOrigin.x + metrics.toolbarPaddingX, controlY}, searchWidth, "##BuildOutputSearch",
                                        m_search, context.localization.Get("editor", "workspace.global_dock.build_output.search"),
                                        context.theme.fonts);
        if (previousSearch != std::string_view{m_search.data()})
            m_filterDirty = true;
        if (showExtended) {
            layout.x = x;
            layout.controlY = controlY;
            x = DrawToolbarStatusChips(layout);
            x = DrawToolbarTargets(metrics, context, x, controlY);
        }
        if (DrawGlobalDockToolbarChip({x, controlY}, filterWidth, filterProps, context.theme.fonts))
            ImGui::OpenPopup("##BuildFiltersPopup");
        DrawFilterPopup(context);
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

    float GlobalDockBuildOutputPane::DrawToolbarTargets(const GlobalDockPaneMetrics &metrics, const EditorGuiContext &context, float x,
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
        return x + rebuildWidth + metrics.toolbarGap;
    }

    void GlobalDockBuildOutputPane::DrawFilterPopup(const EditorGuiContext &context) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        ImGui::SetNextWindowSize({280.0F * scale, 0.0F}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("##BuildFiltersPopup"))
            return;
        DrawStatusFilter(context);
        DrawSeverityFilter(context);
        DrawStageFilter(context);
        DrawSessionFilter(context);
        DrawClearFilter(context);
        ImGui::EndPopup();
    }

    void GlobalDockBuildOutputPane::DrawStatusFilter(const EditorGuiContext &context) {
        DrawFilterLabel(context, "workspace.global_dock.build_output.filter.status");
        const std::vector<std::string> statuses{
            context.localization.Get("editor", "workspace.global_dock.build_output.status.all"),
            context.localization.Get("editor", "workspace.global_dock.build_output.status.ok"),
            context.localization.Get("editor", "workspace.global_dock.build_output.status.failed"),
            context.localization.Get("editor", "workspace.global_dock.build_output.status.cached"),
            context.localization.Get("editor", "workspace.global_dock.build_output.status.warning"),
        };
        int statusIndex = 0;
        switch (m_statusFilter) {
            case StatusFilter::All:
                break;
            case StatusFilter::Ok:
                statusIndex = 1;
                break;
            case StatusFilter::Failed:
            case StatusFilter::Errors:
                statusIndex = 2;
                break;
            case StatusFilter::Cached:
                statusIndex = 3;
                break;
            case StatusFilter::Warning:
                statusIndex = 4;
                break;
        }
        if (DrawFilterCombo(context, "##BuildStatusFilter", statusIndex, statuses)) {
            constexpr std::array filters{StatusFilter::All, StatusFilter::Ok, StatusFilter::Errors, StatusFilter::Cached,
                                         StatusFilter::Warning};
            m_statusFilter = filters[static_cast<std::size_t>(statusIndex)];
            m_filterDirty = true;
        }
    }

    void GlobalDockBuildOutputPane::DrawSeverityFilter(const EditorGuiContext &context) {
        DrawFilterLabel(context, "workspace.global_dock.build_output.filter.severity");
        constexpr std::array severities{DiagnosticSeverity::Note, DiagnosticSeverity::Warning, DiagnosticSeverity::Error,
                                        DiagnosticSeverity::Fatal};
        const std::vector<std::string> severityLabels{
            context.localization.Get("editor", "workspace.global_dock.build_output.filter.any"),
            context.localization.Get("editor", "workspace.global_dock.build_output.row_status.info"),
            context.localization.Get("editor", "workspace.global_dock.build_output.row_status.warning"),
            context.localization.Get("editor", "workspace.global_dock.build_output.row_status.failed"),
            context.localization.Get("editor", "workspace.global_dock.build_output.row_status.fatal"),
        };
        const auto selectedSeverity = std::ranges::find(severities, m_severityFilter.value_or(DiagnosticSeverity::Note));
        int severityIndex = m_severityFilter.has_value() ? static_cast<int>(selectedSeverity - severities.begin()) + 1 : 0;
        if (DrawFilterCombo(context, "##BuildSeverityFilter", severityIndex, severityLabels)) {
            m_severityFilter = severityIndex == 0 ? std::nullopt : std::optional{severities[static_cast<std::size_t>(severityIndex - 1)]};
            m_filterDirty = true;
        }
    }

    void GlobalDockBuildOutputPane::DrawStageFilter(const EditorGuiContext &context) {
        DrawFilterLabel(context, "workspace.global_dock.build_output.filter.stage");
        std::vector<std::string> stages;
        for (const BuildOutputRecord &record : m_snapshot.records) {
            if (!record.stage.empty())
                stages.push_back(record.stage);
        }
        if (!m_stageFilter.empty())
            stages.push_back(m_stageFilter);
        std::ranges::sort(stages);
        stages.erase(std::unique(stages.begin(), stages.end()), stages.end());
        int stageIndex = 0;
        if (!m_stageFilter.empty()) {
            const auto selected = std::ranges::find(stages, m_stageFilter);
            if (selected != stages.end())
                stageIndex = static_cast<int>(selected - stages.begin()) + 1;
        }
        stages.insert(stages.begin(), context.localization.Get("editor", "workspace.global_dock.build_output.filter.any"));
        if (DrawFilterCombo(context, "##BuildStageFilter", stageIndex, stages)) {
            m_stageFilter = stageIndex == 0 ? std::string{} : stages[static_cast<std::size_t>(stageIndex)];
            m_filterDirty = true;
        }
    }

    void GlobalDockBuildOutputPane::DrawSessionFilter(const EditorGuiContext &context) {
        DrawFilterLabel(context, "workspace.global_dock.build_output.filter.session");
        std::vector<BuildOutputSessionId> sessions;
        for (const BuildOutputRecord &record : m_snapshot.records) {
            if (record.sessionId.has_value())
                sessions.push_back(*record.sessionId);
        }
        if (m_sessionFilter.has_value())
            sessions.push_back(*m_sessionFilter);
        std::ranges::sort(sessions);
        sessions.erase(std::unique(sessions.begin(), sessions.end()), sessions.end());
        std::vector<std::string> sessionLabels{context.localization.Get("editor", "workspace.global_dock.build_output.filter.any")};
        int sessionIndex = 0;
        for (std::size_t index = 0; index < sessions.size(); ++index) {
            sessionLabels.push_back(std::format("#{}", sessions[index].Value()));
            if (m_sessionFilter == sessions[index])
                sessionIndex = static_cast<int>(index) + 1;
        }
        if (DrawFilterCombo(context, "##BuildSessionFilter", sessionIndex, sessionLabels)) {
            m_sessionFilter = sessionIndex == 0 ? std::nullopt : std::optional{sessions[static_cast<std::size_t>(sessionIndex - 1)]};
            m_filterDirty = true;
        }
    }

    void GlobalDockBuildOutputPane::DrawClearFilter(const EditorGuiContext &context) {
        ImGui::Separator();
        if (m_snapshot.droppedRecordCount != 0U) {
            const std::string dropped =
                std::format("{} {}", m_snapshot.droppedRecordCount,
                            context.localization.Get("editor", "workspace.global_dock.build_output.footer.dropped"));
            ImGui::TextUnformatted(dropped.c_str());
        }
        if (Ui::Button({.label = context.localization.Get("editor", "workspace.global_dock.build_output.clear").c_str(),
                        .size = {ImGui::GetContentRegionAvail().x, 30.0F * std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F)},
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = !m_snapshot.records.empty(),
                        .font = context.theme.fonts.sans})) {
            m_clearBeforeSequence = m_snapshot.records.back().sequence;
            m_selectedSequence.reset();
            m_filterDirty = true;
            ImGui::CloseCurrentPopup();
        }
    }

}  // namespace Horo::Editor
