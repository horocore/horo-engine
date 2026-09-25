#pragma once

#include <array>

struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief MCP status story hosted by the global dock. */
    class GlobalDockMcpPane {
    public:
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

    private:
        struct TableLayout;

        void DrawToolbar(const ImVec2 &origin, float width, const EditorGuiContext &context);
        [[nodiscard]] float MeasureToolbarActions(const EditorGuiContext &context) const;
        void DrawFilterActions(float &x, float y, const EditorGuiContext &context);
        void DrawSessionActions(float x, float y, const EditorGuiContext &context);
        void DrawTable(const ImVec2 &origin, float width, float height, const EditorGuiContext &context);
        void DrawAuditRow(std::size_t index, float width, const TableLayout &layout, const EditorGuiContext &context);

        enum class Filter : unsigned char {
            All,
            Mutations,
            Errors,
        };

        std::array<char, 256> m_search{};
        Filter m_filter{Filter::All};
        int m_sessionSelection{};
        bool m_paused{};
    };
}  // namespace Horo::Editor
