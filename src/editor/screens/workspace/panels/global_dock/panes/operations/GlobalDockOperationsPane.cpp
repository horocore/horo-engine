#include "editor/screens/workspace/panels/global_dock/panes/operations/GlobalDockOperationsPane.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] GlobalDockTone StatusTone(const OperationState state) noexcept {
            using enum GlobalDockTone;
            using enum OperationState;
            switch (state) {
                case Running:
                case Succeeded:
                    return Positive;
                case Waiting:
                case Cancelling:
                case Cancelled:
                    return Warning;
                case Failed:
                    return Error;
                case Queued:
                    return Neutral;
            }
            return Neutral;
        }

        [[nodiscard]] const char *TechnicalStatusText(const OperationState state) noexcept {
            using enum OperationState;
            switch (state) {
                case Queued:
                    return "QUEUED";
                case Running:
                    return "RUNNING";
                case Waiting:
                    return "WAITING";
                case Cancelling:
                    return "CANCELLING";
                case Succeeded:
                    return "OK";
                case Failed:
                    return "FAILED";
                case Cancelled:
                    return "CANCELLED";
            }
            return "QUEUED";
        }

        [[nodiscard]] const char *StatusLocalizationKey(const OperationState state) noexcept {
            using enum OperationState;
            switch (state) {
                case Queued:
                    return "workspace.global_dock.operations.state.queued";
                case Running:
                    return "workspace.global_dock.operations.state.running";
                case Waiting:
                    return "workspace.global_dock.operations.state.waiting";
                case Cancelling:
                    return "workspace.global_dock.operations.state.cancelling";
                case Succeeded:
                    return "workspace.global_dock.operations.state.succeeded";
                case Failed:
                    return "workspace.global_dock.operations.state.failed";
                case Cancelled:
                    return "workspace.global_dock.operations.state.cancelled";
            }
            return "workspace.global_dock.operations.state.queued";
        }

        [[nodiscard]] std::string OperationTitle(const OperationRecord &operation, const EditorGuiContext &context) {
            if (!operation.title.empty())
                return operation.title;
            using enum OperationKind;
            switch (operation.kind) {
                case Build:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.build");
                case Cook:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.cook");
                case Import:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.import");
                case Index:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.index");
                case Validation:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.validation");
                case Other:
                    return context.localization.Get("editor", "workspace.global_dock.operations.kind.other");
            }
            return {};
        }

        [[nodiscard]] bool ContainsCaseInsensitive(const std::string_view text, const std::string_view needle) {
            if (needle.empty())
                return true;
            if (needle.size() > text.size())
                return false;
            return std::ranges::search(text, needle, [](const char left, const char right) {
                return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
            }).begin() != text.end();
        }

        [[nodiscard]] std::string FormatProgress(const std::optional<float> progress) {
            if (!progress.has_value())
                return "—";
            return std::format("{}%", static_cast<int>(std::clamp(*progress, 0.0F, 1.0F) * 100.0F + 0.5F));
        }

        [[nodiscard]] std::string OperationProgressLabel(const OperationRecord &operation) {
            if (!operation.message.empty())
                return operation.message;
            if (!operation.phase.empty())
                return operation.phase;
            return FormatProgress(operation.progress);
        }

        [[nodiscard]] std::string FormatElapsed(const OperationRecord &operation) {
            if (operation.startedAt == std::chrono::steady_clock::time_point{})
                return "—";
            const auto end = operation.finishedAt.value_or(std::chrono::steady_clock::now());
            const auto elapsed =
                std::max(std::chrono::seconds{0}, std::chrono::duration_cast<std::chrono::seconds>(end - operation.startedAt));
            const long long totalSeconds = elapsed.count();
            return std::format("{:02}:{:02}", totalSeconds / 60, totalSeconds % 60);
        }

        [[nodiscard]] bool IsRunningState(const OperationState state) noexcept {
            using enum OperationState;
            return state == Running || state == Waiting || state == Cancelling;
        }

        [[nodiscard]] bool CanCancel(const OperationRecord &operation) noexcept {
            using enum OperationState;
            return operation.cancellable && operation.state != Cancelling && operation.state != Succeeded && operation.state != Failed &&
                   operation.state != Cancelled;
        }

        [[nodiscard]] const char *ActionKey(const OperationRecord &operation) noexcept {
            if (operation.state == OperationState::Failed)
                return "workspace.global_dock.operations.details";
            if (operation.state == OperationState::Queued)
                return "workspace.global_dock.operations.remove";
            return "workspace.global_dock.operations.cancel";
        }
    }  // namespace

    /** @copydoc GlobalDockOperationsPane::Attach */
    void GlobalDockOperationsPane::Attach(const IOperationQuery *operationQuery, IOperationControl *operationControl) noexcept {
        m_operationQuery = operationQuery;
        m_operationControl = operationControl;
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
        m_stateFilter = StateFilter::All;
        m_kindSelection = 0;
        m_search.fill('\0');
        m_filterDirty = true;
        m_initialFollowTail = true;
    }

    /** @copydoc GlobalDockOperationsPane::Detach */
    void GlobalDockOperationsPane::Detach() noexcept {
        m_operationQuery = nullptr;
        m_operationControl = nullptr;
        m_snapshot = {};
        m_revision = 0;
        m_filteredIndices.clear();
    }

    struct GlobalDockOperationsPane::TableLayout {
        float operation;
        float state;
        float progress;
        float elapsed;
        float action;
        float actionWidth;
    };

    GlobalDockOperationsPane::OperationCounts GlobalDockOperationsPane::CountStates() const noexcept {
        OperationCounts counts;
        for (const OperationRecord &operation : m_snapshot.operations) {
            counts.running += IsRunningState(operation.state) ? 1U : 0U;
            counts.queued += operation.state == OperationState::Queued ? 1U : 0U;
            counts.failed += operation.state == OperationState::Failed ? 1U : 0U;
        }
        return counts;
    }

    float GlobalDockOperationsPane::MeasureToolbarFixedWidth(const OperationCounts &counts, const EditorGuiContext &context) const {
        using enum StateFilter;
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const GlobalDockToolbarChipProps all{.id = "OperationsAll",
                                             .label = context.localization.Get("editor", "workspace.global_dock.operations.filter.all"),
                                             .active = m_stateFilter == All};
        const GlobalDockToolbarChipProps running{.id = "OperationsRunning",
                                                 .label =
                                                     context.localization.Get("editor", "workspace.global_dock.operations.filter.running"),
                                                 .count = counts.running,
                                                 .tone = GlobalDockTone::Accent,
                                                 .active = m_stateFilter == Running};
        const GlobalDockToolbarChipProps failed{.id = "OperationsFailed",
                                                .label =
                                                    context.localization.Get("editor", "workspace.global_dock.operations.filter.failed"),
                                                .count = counts.failed,
                                                .tone = GlobalDockTone::Error,
                                                .active = m_stateFilter == Failed};
        const GlobalDockToolbarChipProps cancelAll{.id = "OperationsCancelAll",
                                                   .label =
                                                       context.localization.Get("editor", "workspace.global_dock.operations.cancel_all"),
                                                   .tone = GlobalDockTone::Error,
                                                   .toneLabel = true,
                                                   .icon = Ui::UiIcon::Delete};
        return MeasureGlobalDockToolbarChip(all, fonts) + MeasureGlobalDockToolbarChip(running, fonts) +
               MeasureGlobalDockToolbarChip(failed, fonts) + 112.0F * scale + MeasureGlobalDockToolbarChip(cancelAll, fonts) +
               metrics.toolbarGap * 6.0F + scale;
    }

    float GlobalDockOperationsPane::DrawStateFilterChips(const float x, const float y, const OperationCounts &counts,
                                                         const EditorGuiContext &context) {
        using enum StateFilter;
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const std::array<GlobalDockToolbarChipProps, 3> filters{
            GlobalDockToolbarChipProps{.id = "OperationsAll",
                                       .label = context.localization.Get("editor", "workspace.global_dock.operations.filter.all"),
                                       .active = m_stateFilter == All},
            GlobalDockToolbarChipProps{.id = "OperationsRunning",
                                       .label = context.localization.Get("editor", "workspace.global_dock.operations.filter.running"),
                                       .count = counts.running,
                                       .tone = GlobalDockTone::Accent,
                                       .active = m_stateFilter == Running},
            GlobalDockToolbarChipProps{.id = "OperationsFailed",
                                       .label = context.localization.Get("editor", "workspace.global_dock.operations.filter.failed"),
                                       .count = counts.failed,
                                       .tone = GlobalDockTone::Error,
                                       .active = m_stateFilter == Failed},
        };
        const std::array states{All, Running, Failed};
        float nextX = x;
        for (std::size_t index = 0; index < std::size(filters); ++index) {
            const float width = MeasureGlobalDockToolbarChip(filters[index], fonts);
            if (DrawGlobalDockToolbarChip({nextX, y}, width, filters[index], fonts)) {
                m_stateFilter = states[index];
                m_filterDirty = true;
            }
            nextX += width + metrics.toolbarGap;
        }
        DrawGlobalDockToolbarSeparator(nextX, y);
        return nextX + metrics.toolbarGap + std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
    }

    void GlobalDockOperationsPane::DrawKindAndCancelActions(const float x, const float y, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float typeWidth = 112.0F * scale;
        const GlobalDockToolbarChipProps cancelAll{.id = "OperationsCancelAll",
                                                   .label =
                                                       context.localization.Get("editor", "workspace.global_dock.operations.cancel_all"),
                                                   .tone = GlobalDockTone::Error,
                                                   .toneLabel = true,
                                                   .icon = Ui::UiIcon::Delete};
        const float cancelAllWidth = MeasureGlobalDockToolbarChip(cancelAll, fonts);
        const std::array<std::string, 7> kindText{
            context.localization.Get("editor", "workspace.global_dock.operations.filter.all_types"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.build"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.cook"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.import"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.index"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.validation"),
            context.localization.Get("editor", "workspace.global_dock.operations.kind.other"),
        };
        const std::array<const char *, 7> kindItems{kindText[0].c_str(), kindText[1].c_str(), kindText[2].c_str(), kindText[3].c_str(),
                                                    kindText[4].c_str(), kindText[5].c_str(), kindText[6].c_str()};
        ImGui::SetCursorScreenPos({x, y});
        ImGui::SetNextItemWidth(typeWidth);
        if (Ui::ComboControl("OperationsType", &m_kindSelection, kindItems.data(), static_cast<int>(kindItems.size()), fonts,
                             {.height = GlobalDockLayout::ControlHeight,
                              .componentSize = Ui::ComponentSize::Small,
                              .surface = Ui::ComboControlSurface::BottomDockToolbar}))
            m_filterDirty = true;
        const float cancelX = x + typeWidth + metrics.toolbarGap;
        if (DrawGlobalDockToolbarChip({cancelX, y}, cancelAllWidth, cancelAll, fonts) && m_operationControl != nullptr) {
            for (const OperationRecord &operation : m_snapshot.operations) {
                if (CanCancel(operation))
                    static_cast<void>(m_operationControl->RequestCancel(operation.id));
            }
        }
    }

    void GlobalDockOperationsPane::DrawToolbar(const GlobalDockPaneRegions &regions, const OperationCounts &counts,
                                               const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        DrawGlobalDockToolbarSurface(regions.toolbarOrigin, regions.toolbarWidth, metrics.toolbarHeight);
        const float controlY = regions.toolbarOrigin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        const float searchWidth =
            std::max(180.0F * scale, regions.toolbarWidth - metrics.toolbarPaddingX * 2.0F - MeasureToolbarFixedWidth(counts, context));
        const float searchX = regions.toolbarOrigin.x + metrics.toolbarPaddingX;

        ImGui::SetCursorScreenPos({searchX, controlY});
        if (const std::string &searchHint = context.localization.Get("editor", "workspace.global_dock.operations.search");
            Ui::InputTextControl("##OperationsSearch", m_search.data(), m_search.size(), fonts,
                                 {.width = searchWidth / scale,
                                  .hint = searchHint.c_str(),
                                  .prefixIconWidth = 20.0F,
                                  .componentSize = Ui::ComponentSize::Small,
                                  .surface = Ui::InputTextSurface::BottomDockToolbar})) {
            m_filterDirty = true;
        }
        Ui::DrawEditorIcon(ImGui::GetWindowDrawList(), Ui::UiIcon::Search, {searchX + 8.0F * scale, controlY + 8.0F * scale},
                           {14.0F * scale, 14.0F * scale}, Theme::U32(Theme::Dim()), fonts.icon);
        const float actionsX = DrawStateFilterChips(searchX + searchWidth + metrics.toolbarGap, controlY, counts, context);
        DrawKindAndCancelActions(actionsX, controlY, context);
    }

    void GlobalDockOperationsPane::DrawTable(const GlobalDockPaneRegions &regions, const bool snapshotChanged,
                                             const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = regions.contentOrigin;
        DrawGlobalDockTableHeaderSurface(headerMin, regions.contentWidth, metrics.tableHeaderHeight);
        const float actionWidth = 72.0F * scale;
        const float elapsedWidth = 70.0F * scale;
        const float actionX = headerMin.x + regions.contentWidth - metrics.contentPadding - actionWidth;
        const TableLayout layout{.operation = headerMin.x + metrics.contentPadding,
                                 .state = headerMin.x + metrics.contentPadding + 120.0F * scale + metrics.columnGap,
                                 .progress = headerMin.x + metrics.contentPadding + 220.0F * scale + metrics.columnGap * 2.0F,
                                 .elapsed = actionX - metrics.columnGap - elapsedWidth,
                                 .action = actionX,
                                 .actionWidth = actionWidth};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              context.localization.Get("editor", key).c_str());
        };
        headerText(layout.operation, "workspace.global_dock.operations.column.operation");
        headerText(layout.state, "workspace.global_dock.operations.column.status");
        headerText(layout.progress, "workspace.global_dock.operations.column.progress");
        headerText(layout.elapsed, "workspace.global_dock.operations.column.elapsed");
        headerText(layout.action, "workspace.global_dock.operations.column.action");

        const ImVec2 rowsOrigin{regions.contentOrigin.x, regions.contentOrigin.y + metrics.tableHeaderHeight};
        const float rowsHeight = std::max(1.0F, regions.contentHeight - metrics.tableHeaderHeight);
        ImGui::SetCursorScreenPos(rowsOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::BeginChild("##OperationsRows", {regions.contentWidth, rowsHeight}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        const bool wasAtBottom = ImGui::GetScrollY() >= std::max(0.0F, ImGui::GetScrollMaxY() - 2.0F);
        for (std::size_t visibleIndex = 0; visibleIndex < m_filteredIndices.size(); ++visibleIndex)
            DrawOperationRow(m_snapshot.operations[m_filteredIndices[visibleIndex]], visibleIndex, regions.contentWidth, layout, context);
        if (snapshotChanged && (wasAtBottom || m_initialFollowTail))
            ImGui::SetScrollHereY(1.0F);
        m_initialFollowTail = false;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void GlobalDockOperationsPane::DrawOperationRow(const OperationRecord &operation, const std::size_t visibleIndex, const float width,
                                                    const TableLayout &layout, const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(visibleIndex));
        ImGui::InvisibleButton("##operation", {width, metrics.tableRowHeight});
        if (const bool hovered = ImGui::IsItemHovered(); hovered)
            drawList->AddRectFilled(rowMin, {rowMin.x + width, rowMin.y + metrics.tableRowHeight}, Theme::U32(Theme::Hover()));
        drawList->AddLine({rowMin.x, rowMin.y + metrics.tableRowHeight - scale},
                          {rowMin.x + width, rowMin.y + metrics.tableRowHeight - scale}, Theme::U32(Theme::Border()));
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.operation, textY},
                                  {layout.state - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(),
                                  OperationTitle(operation, context));
        const std::string stateLabel = context.localization.Get("editor", StatusLocalizationKey(operation.state));
        static_cast<void>(DrawGlobalDockStatePill({layout.state, rowMin.y + (metrics.tableRowHeight - 22.0F * scale) * 0.5F}, stateLabel,
                                                  StatusTone(operation.state), fonts));
        const float progressRight = layout.elapsed - metrics.columnGap;
        const std::string progressLabel = OperationProgressLabel(operation);
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Caption(), {layout.progress, textY},
                                  {progressRight, rowMin.y + metrics.tableRowHeight}, Theme::Muted(), progressLabel);
        if (operation.progress.has_value())
            DrawGlobalDockProgressBar({layout.progress, rowMin.y + metrics.tableRowHeight - 8.0F * scale},
                                      std::max(1.0F, progressRight - layout.progress), *operation.progress);
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.elapsed, textY},
                                  {layout.action - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(),
                                  FormatElapsed(operation));
        DrawOperationAction(operation, rowMin, layout, context);
        ImGui::PopID();
    }

    void GlobalDockOperationsPane::DrawOperationAction(const OperationRecord &operation, const ImVec2 rowMinimum, const TableLayout &layout,
                                                       const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const bool canCancel = CanCancel(operation);
        if (const bool showDetails = operation.state == OperationState::Failed; canCancel || showDetails) {
            const GlobalDockToolbarChipProps action{.id = "OperationAction",
                                                    .label = context.localization.Get("editor", ActionKey(operation)),
                                                    .tone = canCancel ? GlobalDockTone::Error : GlobalDockTone::Neutral,
                                                    .toneLabel = canCancel};
            if (DrawGlobalDockToolbarChip({layout.action, rowMinimum.y + (metrics.tableRowHeight - metrics.controlHeight) * 0.5F},
                                          layout.actionWidth, action, fonts) &&
                canCancel && m_operationControl != nullptr)
                static_cast<void>(m_operationControl->RequestCancel(operation.id));
        }
    }


    /** @copydoc GlobalDockOperationsPane::Draw */
    void GlobalDockOperationsPane::Draw(const ImVec2 &contentOrigin, const float contentWidth, const EditorGuiContext &context) {
        const bool snapshotChanged = RefreshSnapshot();
        if (m_filterDirty)
            RebuildFilter();
        const float availableHeight = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - contentOrigin.y);
        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions(contentOrigin, contentWidth, availableHeight, {.hasToolbar = true});
        const OperationCounts counts = CountStates();
        DrawToolbar(regions, counts, context);
        DrawTable(regions, snapshotChanged, context);
    }

    bool GlobalDockOperationsPane::RefreshSnapshot() {
        if (m_operationQuery == nullptr)
            return false;
        auto changed = m_operationQuery->SnapshotIfChanged(m_revision);
        if (!changed.has_value())
            return false;
        m_snapshot = std::move(*changed);
        m_revision = m_snapshot.revision;
        m_filterDirty = true;
        return true;
    }

    std::vector<std::size_t> GlobalDockOperationsPane::ProjectRecords(const std::span<const OperationRecord> operations,
                                                                      const std::string_view search) {
        std::vector<std::size_t> projected;
        projected.reserve(operations.size());
        for (std::size_t index = 0; index < operations.size(); ++index) {
            if (const OperationRecord &operation = operations[index];
                !search.empty() && !ContainsCaseInsensitive(operation.title, search) && !ContainsCaseInsensitive(operation.phase, search) &&
                !ContainsCaseInsensitive(operation.message, search) &&
                !ContainsCaseInsensitive(TechnicalStatusText(operation.state), search))
                continue;
            projected.push_back(index);
        }
        return projected;
    }

    void GlobalDockOperationsPane::RebuildFilter() {
        const std::vector<std::size_t> searched = ProjectRecords(m_snapshot.operations, std::string_view{m_search.data()});
        m_filteredIndices.clear();
        m_filteredIndices.reserve(searched.size());
        for (const std::size_t index : searched) {
            const OperationRecord &operation = m_snapshot.operations[index];
            const bool stateMatches = m_stateFilter == StateFilter::All ||
                                      (m_stateFilter == StateFilter::Running && IsRunningState(operation.state)) ||
                                      (m_stateFilter == StateFilter::Failed && operation.state == OperationState::Failed);
            const bool kindMatches = m_kindSelection == 0 || static_cast<int>(operation.kind) + 1 == m_kindSelection;
            if (stateMatches && kindMatches)
                m_filteredIndices.push_back(index);
        }
        m_filterDirty = false;
    }
}  // namespace Horo::Editor
