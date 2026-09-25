#pragma once

#include <array>

struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Network status story hosted by the global dock. */
    class GlobalDockNetworkPane {
    public:
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

    private:
        struct TableLayout;

        void DrawToolbar(const ImVec2 &origin, float width, const EditorGuiContext &context);
        [[nodiscard]] float MeasureToolbarActions(const EditorGuiContext &context) const;
        void DrawToolbarActions(float x, float y, const EditorGuiContext &context);
        void DrawSessionSelector(float &x, float y, const EditorGuiContext &context);
        [[nodiscard]] float DrawMetrics(const ImVec2 &origin, float width, const EditorGuiContext &context) const;
        void DrawTable(const ImVec2 &origin, float width, float height, const EditorGuiContext &context) const;
        void DrawRow(std::size_t index, float width, const TableLayout &layout, const EditorGuiContext &context) const;

        std::array<char, 256> m_search{};
        int m_sessionSelection{};
        int m_viewSelection{};
        bool m_paused{};
        bool m_cleared{};
    };
}  // namespace Horo::Editor
