#pragma once

#include "Horo/Editor/Localization/LocalizedGui.h"
#include "Horo/Editor/WorkspaceMcpControls.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ImVec2;

namespace Horo::Log {
    class IStructuredLogQuery;
    struct StructuredLogRecord;
}  // namespace Horo::Log

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Live, redacted MCP history hosted by the global dock. */
    class GlobalDockMcpPane : public LocalizedGui {
    public:
        /** @brief Catalog scope for this embedded dock surface. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.global_dock.mcp"};

        GlobalDockMcpPane() : LocalizedGui(kTextScope) {}

        /** @brief Binds the host's log and local controls.
         * @param query Borrowed history query. @param controls Optional local editor control port.
         */
        void Attach(const Log::IStructuredLogQuery *query, IWorkspaceMcpControls *controls = nullptr) noexcept;
        /** @brief Drops the workspace query and all cached presentation state. */
        void Detach() noexcept;
        /** @brief Reveals only records for the exact operation and resumes live history. */
        void FocusAudit(WorkspaceMcpAuditReference reference) noexcept;
        /** @brief Draws the filtered history and its local export action. */
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

    private:
        enum class Filter : std::uint8_t {
            All,
            Approvals,
            Errors
        };

        /** @brief Immutable, validated projection shared by display and export. */
        struct Row {
            std::chrono::system_clock::time_point timestamp;
            std::uint64_t principal{};
            std::uint64_t session{};
            std::uint64_t generation{};
            std::uint64_t operation{};
            std::uint64_t approval{};
            std::int64_t elapsed{};
            std::string_view event;
            std::string_view status;
            std::string tool;
            std::string time;
            std::string scope;
        };

        /** @brief Refreshes at most the newest bounded set when the log revision changes. */
        void Refresh();
        /** @brief Admits only the known audit schema and ignores unrelated log content. */
        void AppendRow(const Log::StructuredLogRecord &record);
        /** @brief Measures translated controls against the available toolbar width. */
        void DrawToolbar(const ImVec2 &origin, float width, const EditorGuiContext &context);
        /** @brief Draws live capabilities and controller-owned actions independently of history. */
        void DrawSession(float width, const EditorGuiContext &context);
        /** @brief Presents a single active request and routes cancellation to its authenticated owner. */
        void DrawRequest(const WorkspaceMcpRequest &request, float width, const EditorGuiContext &context);
        /** @brief Lays out the audit columns and presents the admitted rows. */
        void DrawTable(const ImVec2 &origin, float width, float height, const EditorGuiContext &context);
        /** @brief Draws the filtered records or the appropriate empty state. */
        void DrawHistory(float width, const std::array<float, 6> &columns, const EditorGuiContext &context) const;
        /** @brief Draws a validated row using bounded column clipping. */
        void DrawRow(const Row &row, float width, const std::array<float, 6> &columns, const EditorGuiContext &context) const;
        /** @brief Applies the shared display and export filter. */
        [[nodiscard]] bool Matches(const Row &row, const EditorGuiContext &context) const;
        /** @brief Copies the current filtered snapshot using numeric IDs and canonical labels. */
        void CopyVisible(const EditorGuiContext &context) const;

        std::array<char, 256> m_search{};
        const Log::IStructuredLogQuery *m_query{};
        IWorkspaceMcpControls *m_controls{};
        WorkspaceMcpStatus m_live;
        bool m_controlError{};
        std::uint64_t m_revision{};
        std::vector<Row> m_rows;
        std::optional<WorkspaceMcpAuditReference> m_audit;
        Filter m_filter{Filter::All};
        bool m_paused{};
    };
}  // namespace Horo::Editor
