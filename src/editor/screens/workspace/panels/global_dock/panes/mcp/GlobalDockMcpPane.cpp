#include "editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <numeric>
#include <optional>

namespace Horo::Editor {
    namespace {
        constexpr std::array<std::string_view, 9> Events{
            "session.opened",     "session.retired",  "approval.requested", "approval.decided",   "approval.consumed",
            "operation.rejected", "operation.queued", "operation.started",  "operation.finished",
        };
        constexpr std::array<std::string_view, 13> Statuses{
            "",       "closed",  "project_changed", "shutdown", "approved",  "denied",    "authorization_denied",
            "queued", "running", "succeeded",       "failed",   "cancelled", "timed_out",
        };
        constexpr std::size_t MaximumRows = 256;

        /** @brief Borrows only a field with the expected telemetry type. */
        template <typename T> const T *Field(const Log::StructuredLogRecord &record, const std::string_view key) {
            const auto found = std::ranges::find(record.fields, key, &Telemetry::Field::key);
            return found == record.fields.end() || found->privacy != Telemetry::FieldPrivacy::Public ? nullptr
                                                                                                     : std::get_if<T>(&found->value);
        }

        /** @brief Returns zero only for omitted or inapplicable correlations. */
        std::uint64_t Number(const Log::StructuredLogRecord &record, const std::string_view key) {
            const auto *value = Field<std::uint64_t>(record, key);
            return value ? *value : 0U;
        }

        /** @brief Ensures a registry token contains no path, delimiter, or spreadsheet formula prefix. */
        bool ToolToken(const std::string_view value) {
            constexpr std::string_view alphanumeric = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            constexpr std::string_view punctuation = "._-";
            return value.empty() || (value.size() <= 128 && alphanumeric.find(value.front()) != std::string_view::npos &&
                                     std::ranges::all_of(value, [&](const char c) {
                return alphanumeric.find(c) != std::string_view::npos || punctuation.find(c) != std::string_view::npos;
            }));
        }

        /** @brief Canonical labels and the borrowed, validated registry token. */
        struct AuditLabels {
            std::string_view event;
            std::string_view status;
            std::string_view tool;
        };

        /** @brief Resolves the input fields to the supported audit vocabulary. */
        std::optional<AuditLabels> ReadLabels(const Log::StructuredLogRecord &record) {
            const auto event = std::ranges::find(Events, record.message);
            const auto *tool = Field<std::string>(record, "tool");
            const auto *text = Field<std::string>(record, "status");
            if (event == Events.end() || !tool || !ToolToken(*tool) || !text)
                return std::nullopt;
            const auto status = std::ranges::find(Statuses, *text);
            if (status == Statuses.end())
                return std::nullopt;
            return AuditLabels{*event, *status, *tool};
        }

        /** @brief Formats the retained UTC timestamp independently of the host locale. */
        std::string TimeOfDay(const std::chrono::system_clock::time_point timestamp) {
            const std::time_t converted = std::chrono::system_clock::to_time_t(timestamp);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &converted);
#else
            gmtime_r(&converted, &utc);
#endif
            return std::format("{:02d}:{:02d}:{:02d}", utc.tm_hour, utc.tm_min, utc.tm_sec);
        }

        /** @brief Resolves only canonical metadata names through the editor catalog. */
        const std::string &Text(const EditorGuiContext &context, const std::string_view key) {
            return GlobalDockMcpPane::kTextScope.Text(context.localization, key);
        }

        /** @brief Classifies canonical terminal and admission failures for the error filter. */
        bool IsError(const std::string_view status) noexcept {
            return status == "failed" || status == "timed_out" || status.ends_with("denied");
        }
    }  // namespace

    /** @copydoc GlobalDockMcpPane::Attach */
    void GlobalDockMcpPane::Attach(const Log::IStructuredLogQuery *query, IWorkspaceMcpControls *controls) noexcept {
        Detach();
        m_query = query;
        m_controls = controls;
    }

    /** @copydoc GlobalDockMcpPane::Detach */
    void GlobalDockMcpPane::Detach() noexcept {
        m_query = nullptr;
        m_controls = nullptr;
        m_live = {};
        m_controlError = false;
        m_revision = 0;
        m_rows.clear();
        m_audit.reset();
        m_paused = false;
        m_search.fill('\0');
        m_filter = Filter::All;
    }

    /** @copydoc GlobalDockMcpPane::FocusAudit */
    void GlobalDockMcpPane::FocusAudit(const WorkspaceMcpAuditReference reference) noexcept {
        m_audit = reference;
        m_paused = false;
        m_search.fill('\0');
        m_filter = Filter::All;
    }

    /** @copydoc GlobalDockMcpPane::Draw */
    void GlobalDockMcpPane::Draw(const ImVec2 &origin, const float width, const EditorGuiContext &context) {
        Refresh();
        if (m_controls) {
            if (auto live = m_controls->ReadMcpStatus(m_live.revision))
                m_live = std::move(*live);
        }
        const float available = std::max(1.0F, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - origin.y);
        const auto regions = ResolveGlobalDockPaneRegions(origin, width, available, {.hasToolbar = true});
        DrawToolbar(regions.toolbarOrigin, regions.toolbarWidth, context);
        DrawTable(regions.contentOrigin, regions.contentWidth, regions.contentHeight, context);
    }

    /** @copydoc GlobalDockMcpPane::Refresh */
    void GlobalDockMcpPane::Refresh() {
        if (!m_query || m_paused)
            return;
        const auto snapshot = m_query->SnapshotIfChanged(m_revision);
        if (!snapshot)
            return;
        m_revision = snapshot->revision;
        m_rows.clear();
        for (auto row = snapshot->records.rbegin(); row != snapshot->records.rend() && m_rows.size() < MaximumRows; ++row) {
            if (*row)
                AppendRow(**row);
        }
    }

    /** @copydoc GlobalDockMcpPane::AppendRow */
    void GlobalDockMcpPane::AppendRow(const Log::StructuredLogRecord &record) {
        if (record.category != "mcp.audit" || Number(record, "schema_version") != 1)
            return;
        const auto labels = ReadLabels(record);
        const auto principal = Number(record, "principal_id");
        const auto session = Number(record, "session_id");
        const auto generation = Number(record, "session_generation");
        if (!labels || !principal || !session || !generation)
            return;
        const auto operation = Number(record, "operation_id");
        const auto approval = Number(record, "approval_id");
        const auto *elapsed = Field<std::int64_t>(record, "elapsed_ms");
        m_rows.push_back({record.timestampUtc, principal, session, generation, operation, approval, elapsed ? *elapsed : 0, labels->event,
                          labels->status, std::string{labels->tool}, TimeOfDay(record.timestampUtc),
                          std::format("{} / {}.{}", principal, session, generation)});
    }

    /** @copydoc GlobalDockMcpPane::DrawToolbar */
    void GlobalDockMcpPane::DrawToolbar(const ImVec2 &origin, const float width, const EditorGuiContext &context) {
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const auto &fonts = context.theme.fonts;
        DrawGlobalDockToolbarSurface(origin, width, metrics.toolbarHeight);
        const bool linked = m_audit.has_value();
        const std::array actions{
            GlobalDockToolbarChipProps{.id = "McpAll",
                                       .label = Text(context, "filter.all"),
                                       .active = m_filter == Filter::All && !linked,
                                       .disabled = linked},
            GlobalDockToolbarChipProps{.id = "McpApprovals",
                                       .label = Text(context, "filter.approvals"),
                                       .active = m_filter == Filter::Approvals && !linked,
                                       .disabled = linked},
            GlobalDockToolbarChipProps{.id = "McpErrors",
                                       .label = Text(context, "filter.errors"),
                                       .active = m_filter == Filter::Errors && !linked,
                                       .disabled = linked},
            GlobalDockToolbarChipProps{.id = "McpPause", .label = Text(context, m_paused ? "resume" : "pause"), .active = m_paused},
            GlobalDockToolbarChipProps{.id = "McpExport", .label = Text(context, "copy"), .disabled = m_rows.empty()},
        };
        std::array<float, actions.size()> widths{};
        std::ranges::transform(actions, widths.begin(), [&](const auto &action) {
            return MeasureGlobalDockToolbarChip(action, fonts);
        });
        const float total = std::accumulate(widths.begin(), widths.end(), 0.0F);
        const float gaps = metrics.toolbarGap * static_cast<float>(actions.size());
        const float factor = std::clamp((width * 0.76F - gaps) / total, 0.05F, 1.0F);
        const float searchWidth = ResolveGlobalDockSearchWidth(width, total * factor + gaps);
        const float y = origin.y + (metrics.toolbarHeight - metrics.controlHeight) * 0.5F;
        ImGui::BeginDisabled(linked);
        float x = DrawGlobalDockSearchControl({origin.x + metrics.toolbarPaddingX, y}, searchWidth, "##McpSearch", m_search,
                                              Text(context, "search"), fonts);
        ImGui::EndDisabled();
        constexpr std::array filters{Filter::All, Filter::Approvals, Filter::Errors};
        for (std::size_t index = 0; index < actions.size(); ++index) {
            const float actionWidth = widths[index] * factor;
            if (DrawGlobalDockToolbarChip({x, y}, actionWidth, actions[index], fonts)) {
                if (index < filters.size())
                    m_filter = filters[index];
                else if (index == filters.size())
                    m_paused = !m_paused;
                else
                    CopyVisible(context);
            }
            x += actionWidth + metrics.toolbarGap;
        }
    }

    /** @copydoc GlobalDockMcpPane::Matches */
    bool GlobalDockMcpPane::Matches(const Row &row, const EditorGuiContext &context) const {
        if (m_audit)
            return *m_audit == WorkspaceMcpAuditReference{{row.principal, row.session, row.generation}, row.operation};
        if (m_filter == Filter::Approvals && !row.event.starts_with("approval."))
            return false;
        if (m_filter == Filter::Errors && !IsError(row.status))
            return false;
        const std::string_view search{m_search.data()};
        return search.empty() || GlobalDockContainsCaseInsensitive(row.tool, search) ||
               GlobalDockContainsCaseInsensitive(row.scope, search) || GlobalDockContainsCaseInsensitive(Text(context, row.event), search);
    }

    /** @copydoc GlobalDockMcpPane::CopyVisible */
    void GlobalDockMcpPane::CopyVisible(const EditorGuiContext &context) const {
        std::string csv = "utc_ms,principal,session,generation,operation,approval,event,tool,status,elapsed_ms\n";
        for (const auto &row : m_rows) {
            if (Matches(row, context))
                csv += std::format("{},{},{},{},{},{},{},{},{},{}\n",
                                   std::chrono::duration_cast<std::chrono::milliseconds>(row.timestamp.time_since_epoch()).count(),
                                   row.principal, row.session, row.generation, row.operation, row.approval, row.event, row.tool, row.status,
                                   row.elapsed);
        }
        ImGui::SetClipboardText(csv.c_str());
    }

    /** @copydoc GlobalDockMcpPane::DrawSession */
    void GlobalDockMcpPane::DrawSession(const float width, const EditorGuiContext &context) {
        const auto &fonts = context.theme.fonts;
        if (m_controlError)
            DrawGlobalDockWrappedText(width, Text(context, "control.failed"), GlobalDockTone::Error, fonts);
        const auto key = m_live.revision.session;
        if (!m_controls || !key.principal)
            return;
        DrawGlobalDockWrappedText(width, Text(context, "control.session"), GlobalDockTone::Accent, fonts);
        DrawGlobalDockWrappedText(width, std::format("{} / {}.{}", key.principal, key.session, key.generation), GlobalDockTone::Neutral,
                                  fonts);
        if (!m_live.capabilities.empty()) {
            DrawGlobalDockWrappedText(width, Text(context, "control.capabilities"), GlobalDockTone::Neutral, fonts);
            for (const auto &capability : m_live.capabilities)
                DrawGlobalDockWrappedText(width, capability, GlobalDockTone::Neutral, fonts);
        }
        const auto revoke = Text(context, "control.revoke") + "###McpRevokeSession";
        if (Ui::Button({.label = revoke.c_str()})) {
            m_controlError = m_controls->RevokeMcpSession(key).HasError();
            if (!m_controlError)
                m_live = {};
            return;
        }
        for (const auto &request : m_live.requests)
            DrawRequest(request, width, context);
        ImGui::Spacing();
    }

    /** @copydoc GlobalDockMcpPane::DrawRequest */
    void GlobalDockMcpPane::DrawRequest(const WorkspaceMcpRequest &request, const float width, const EditorGuiContext &context) {
        const auto state = Text(context, request.running ? "status.running" : "status.queued");
        DrawGlobalDockWrappedText(width, std::format("#{} {} · {}", request.id, request.tool, state), GlobalDockTone::Neutral,
                                  context.theme.fonts);
        const auto label = std::format("{}###McpCancel{}", Text(context, "control.cancel"), request.id);
        if (Ui::Button({.label = label.c_str()}))
            m_controlError = m_controls->CancelMcpRequest(m_live.revision.session, request.id).HasError();
    }

    /** @copydoc GlobalDockMcpPane::DrawTable */
    void GlobalDockMcpPane::DrawTable(const ImVec2 &origin, const float width, const float height, const EditorGuiContext &context) {
        constexpr std::array keys{"column.time", "column.tool", "column.event", "column.scope", "column.status"};
        constexpr std::array fractions{0.0F, 0.13F, 0.38F, 0.63F, 0.81F, 1.0F};
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const auto &fonts = context.theme.fonts;
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, 0.0F});
        ImGui::BeginChild("##McpRows", {width, std::max(1.0F, height)}, false, ImGuiWindowFlags_NoSavedSettings);
        if (m_audit) {
            const auto clear = Text(context, "audit.show_all") + "###McpClearAudit";
            if (Ui::Button({.label = clear.c_str()}))
                m_audit.reset();
        }
        DrawSession(ImGui::GetContentRegionAvail().x, context);
        const auto header = ImGui::GetCursorScreenPos();
        const float content = std::max(1.0F, width - metrics.contentPadding * 2.0F);
        std::array<float, fractions.size()> columns{};
        std::ranges::transform(fractions, columns.begin(), [&](const float fraction) {
            return header.x + metrics.contentPadding + content * fraction;
        });
        DrawGlobalDockTableHeaderSurface(header, width, metrics.tableHeaderHeight);
        const float bottom = header.y + metrics.tableHeaderHeight;
        const float y = header.y + (metrics.tableHeaderHeight - Theme::TextPx::Caption()) * 0.5F;
        for (std::size_t index = 0; index < keys.size(); ++index)
            DrawGlobalDockClippedText(*ImGui::GetWindowDrawList(), fonts.sansCompact, Theme::TextPx::Caption(), {columns[index], y},
                                      {columns[index + 1], bottom}, Theme::Muted(), Text(context, keys[index]));
        ImGui::Dummy({width, metrics.tableHeaderHeight});
        DrawHistory(width, columns, context);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }

    /** @copydoc GlobalDockMcpPane::DrawHistory */
    void GlobalDockMcpPane::DrawHistory(const float width, const std::array<float, 6> &columns, const EditorGuiContext &context) const {
        bool drewRow = false;
        for (const auto &row : m_rows) {
            if (!Matches(row, context))
                continue;
            DrawRow(row, width, columns, context);
            drewRow = true;
        }
        if (!drewRow) {
            const auto *label = m_audit ? "audit.empty" : m_query ? "empty" : "unavailable";
            DrawGlobalDockWrappedText(width, Text(context, label), GlobalDockTone::Neutral, context.theme.fonts);
        }
    }

    /** @copydoc GlobalDockMcpPane::DrawRow */
    void GlobalDockMcpPane::DrawRow(const Row &row, const float width, const std::array<float, 6> &columns,
                                    const EditorGuiContext &context) const {
        const auto metrics = ResolveGlobalDockPaneMetrics();
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 size{width, metrics.tableRowHeight};
        if (!ImGui::IsRectVisible(size)) {
            ImGui::Dummy(size);
            return;
        }
        DrawGlobalDockTableRowSurface(origin, width, metrics.tableRowHeight,
                                      ImGui::IsMouseHoveringRect(origin, {origin.x + width, origin.y + metrics.tableRowHeight}));
        const std::array<std::string_view, 5> values{row.time, row.tool, Text(context, row.event), row.scope,
                                                     Text(context, "status." + std::string{row.status.empty() ? "none" : row.status})};
        const float y = origin.y + (metrics.tableRowHeight - Theme::TextPx::Label()) * 0.5F;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto color =
                index == values.size() - 1 && IsError(row.status) ? GlobalDockToneColor(GlobalDockTone::Error) : Theme::Text();
            DrawGlobalDockClippedText(*ImGui::GetWindowDrawList(), context.theme.fonts.sansCompact, Theme::TextPx::Label(),
                                      {columns[index], y}, {columns[index + 1] - metrics.columnGap, origin.y + metrics.tableRowHeight},
                                      color, values[index]);
        }
        ImGui::Dummy(size);
    }
}  // namespace Horo::Editor
