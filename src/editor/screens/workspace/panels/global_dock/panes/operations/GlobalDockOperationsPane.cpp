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
                    return "state.queued";
                case Running:
                    return "state.running";
                case Waiting:
                    return "state.waiting";
                case Cancelling:
                    return "state.cancelling";
                case Succeeded:
                    return "state.succeeded";
                case Failed:
                    return "state.failed";
                case Cancelled:
                    return "state.cancelled";
            }
            return "state.queued";
        }

        [[nodiscard]] std::string OperationTitle(const OperationRecord &operation, const EditorGuiContext &context) {
            if (!operation.title.empty())
                return operation.title;
            using enum OperationKind;
            switch (operation.kind) {
                case Build:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.build");
                case Cook:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.cook");
                case Import:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.import");
                case Index:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.index");
                case Validation:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.validation");
                case Other:
                    return GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.other");
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
                return "details";
            if (operation.state == OperationState::Queued)
                return "remove";
            return "cancel";
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
        float message;
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
                                             .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.all"),
                                             .active = m_stateFilter == All};
        const GlobalDockToolbarChipProps running{.id = "OperationsRunning",
                                                 .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.running"),
                                                 .count = counts.running,
                                                 .tone = GlobalDockTone::Accent,
                                                 .active = m_stateFilter == Running};
        const GlobalDockToolbarChipProps failed{.id = "OperationsFailed",
                                                .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.failed"),
                                                .count = counts.failed,
                                                .tone = GlobalDockTone::Error,
                                                .active = m_stateFilter == Failed};
        const GlobalDockToolbarChipProps cancelAll{.id = "OperationsCancelAll",
                                                   .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "cancel_all"),
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
                                       .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.all"),
                                       .active = m_stateFilter == All},
            GlobalDockToolbarChipProps{.id = "OperationsRunning",
                                       .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.running"),
                                       .count = counts.running,
                                       .tone = GlobalDockTone::Accent,
                                       .active = m_stateFilter == Running},
            GlobalDockToolbarChipProps{.id = "OperationsFailed",
                                       .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.failed"),
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
                                                   .label = GlobalDockOperationsPane::kTextScope.Text(context.localization, "cancel_all"),
                                                   .tone = GlobalDockTone::Error,
                                                   .toneLabel = true,
                                                   .icon = Ui::UiIcon::Delete};
        const float cancelAllWidth = MeasureGlobalDockToolbarChip(cancelAll, fonts);
        const std::array<std::string, 7> kindText{
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "filter.all_types"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.build"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.cook"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.import"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.index"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.validation"),
            GlobalDockOperationsPane::kTextScope.Text(context.localization, "kind.other"),
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
        if (const std::string &searchHint = GlobalDockOperationsPane::kTextScope.Text(context.localization, "search");
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
        float stateWidth = 0;
        for (const auto state : {OperationState::Queued, OperationState::Running, OperationState::Waiting, OperationState::Cancelling,
                                 OperationState::Succeeded, OperationState::Failed, OperationState::Cancelled})
            stateWidth =
                std::max(stateWidth, MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Caption(),
                                                                kTextScope.Text(context.localization, StatusLocalizationKey(state))) +
                                         30.0F * scale);
        float actionWidth = 0;
        for (const auto *key : {"details", "cancel", "remove", "column.action"})
            actionWidth = std::max(actionWidth, MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Label(),
                                                                           kTextScope.Text(context.localization, key)) +
                                                    16.0F * scale);
        const float elapsedWidth =
            std::max(70.0F * scale, MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Caption(),
                                                               kTextScope.Text(context.localization, "column.elapsed")));
        const float fixedWidth = metrics.contentPadding * 2 + metrics.columnGap * 4 + stateWidth + elapsedWidth + actionWidth;
        const float tableWidth = std::max(regions.contentWidth - ImGui::GetStyle().ScrollbarSize, fixedWidth + 420.0F * scale);
        ImGui::SetCursorScreenPos(regions.contentOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::SetNextWindowContentSize({tableWidth, 0});
        ImGui::BeginChild("##OperationsRows", {regions.contentWidth, regions.contentHeight}, false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar |
                              ImGuiWindowFlags_NoSavedSettings);
        const bool wasAtBottom = ImGui::GetScrollY() >= std::max(0.0F, ImGui::GetScrollMaxY() - 2.0F);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 headerMin = ImGui::GetCursorScreenPos();
        DrawGlobalDockTableHeaderSurface(headerMin, tableWidth, metrics.tableHeaderHeight);
        const float operationWidth = std::clamp((tableWidth - fixedWidth) * 0.45F, 220.0F * scale, 520.0F * scale);
        const float stateX = headerMin.x + metrics.contentPadding + operationWidth + metrics.columnGap;
        const float actionX = headerMin.x + tableWidth - metrics.contentPadding - actionWidth;
        const TableLayout layout{.operation = headerMin.x + metrics.contentPadding,
                                 .state = stateX,
                                 .message = stateX + stateWidth + metrics.columnGap,
                                 .elapsed = actionX - metrics.columnGap - elapsedWidth,
                                 .action = actionX,
                                 .actionWidth = actionWidth};
        const float headerY = headerMin.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        const auto headerText = [&](const float textX, const char *key) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {textX, headerY}, Theme::U32(Theme::Muted()),
                              GlobalDockOperationsPane::kTextScope.Text(context.localization, key).c_str());
        };
        headerText(layout.operation, "column.operation");
        headerText(layout.state, "column.status");
        headerText(layout.message, "column.message");
        headerText(layout.elapsed, "column.elapsed");
        headerText(layout.action, "column.action");

        ImGui::SetCursorScreenPos({headerMin.x, headerMin.y + metrics.tableHeaderHeight});
        for (std::size_t visibleIndex = 0; visibleIndex < m_filteredIndices.size(); ++visibleIndex)
            DrawOperationRow(m_snapshot.operations[m_filteredIndices[visibleIndex]], visibleIndex, tableWidth, layout, context);
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
        ImGui::Dummy({width, metrics.tableRowHeight});
        const ImVec2 nextRow = ImGui::GetCursorScreenPos();
        if (const bool hovered = ImGui::IsItemHovered(); hovered)
            drawList->AddRectFilled(rowMin, {rowMin.x + width, rowMin.y + metrics.tableRowHeight}, Theme::U32(Theme::Hover()));
        drawList->AddLine({rowMin.x, rowMin.y + metrics.tableRowHeight - scale},
                          {rowMin.x + width, rowMin.y + metrics.tableRowHeight - scale}, Theme::U32(Theme::Border()));
        const float textY = rowMin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        DrawGlobalDockEllipsizedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.operation, textY},
                                     {layout.state - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(),
                                     OperationTitle(operation, context));
        const std::string stateLabel =
            GlobalDockOperationsPane::kTextScope.Text(context.localization, StatusLocalizationKey(operation.state));
        static_cast<void>(DrawGlobalDockStatePill({layout.state, rowMin.y + (metrics.tableRowHeight - 22.0F * scale) * 0.5F}, stateLabel,
                                                  StatusTone(operation.state), fonts));
        const float messageRight = layout.elapsed - metrics.columnGap;
        const bool showProgress = IsRunningState(operation.state) && operation.progress.has_value();
        const float meterWidth = showProgress ? 76.0F * scale : 0;
        const float messageEnd = showProgress ? messageRight - meterWidth - metrics.columnGap : messageRight;
        const std::string message = OperationProgressLabel(operation);
        DrawGlobalDockEllipsizedText(*drawList, fonts.sansCompact, Theme::TextPx::Caption(), {layout.message, textY},
                                     {messageEnd, rowMin.y + metrics.tableRowHeight}, Theme::Muted(), message);
        if (showProgress)
            DrawGlobalDockProgressBar({messageRight - meterWidth, rowMin.y + (metrics.tableRowHeight - 5.0F * scale) * 0.5F}, meterWidth,
                                      *operation.progress);
        DrawGlobalDockClippedText(*drawList, fonts.sansCompact, Theme::TextPx::Label(), {layout.elapsed, textY},
                                  {layout.action - metrics.columnGap, rowMin.y + metrics.tableRowHeight}, Theme::Text(),
                                  FormatElapsed(operation));
        DrawOperationAction(operation, rowMin, layout, context);
        ImGui::SetCursorScreenPos(nextRow);
        ImGui::PopID();
    }

    void GlobalDockOperationsPane::DrawOperationAction(const OperationRecord &operation, const ImVec2 rowMinimum, const TableLayout &layout,
                                                       const EditorGuiContext &context) {
        const Theme::Fonts &fonts = context.theme.fonts;
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const bool canCancel = CanCancel(operation);
        if (canCancel || operation.state == OperationState::Failed) {
            const auto &label = kTextScope.Text(context.localization, ActionKey(operation));
            const float height = std::max(Theme::TextPx::Label(), 20.0F);
            ImGui::SetCursorScreenPos({layout.action, rowMinimum.y + (metrics.tableRowHeight - height) * 0.5F});
            if (Ui::TextLink("OperationAction", label.c_str(), fonts.sansCompact, Theme::TextPx::Label())) {
                if (canCancel && m_operationControl != nullptr)
                    static_cast<void>(m_operationControl->RequestCancel(operation.id));
                else if (operation.state == OperationState::Failed)
                    ImGui::OpenPopup("##OperationDetails");
            }
            if (ImGui::BeginPopup("##OperationDetails")) {
                DrawGlobalDockWrappedText(400.0F * Theme::GetActiveTokens().sizes.uiScale, OperationTitle(operation, context),
                                          GlobalDockTone::Neutral, fonts);
                ImGui::Spacing();
                DrawGlobalDockWrappedText(400.0F * Theme::GetActiveTokens().sizes.uiScale, OperationProgressLabel(operation),
                                          GlobalDockTone::Error, fonts);
                ImGui::EndPopup();
            }
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
