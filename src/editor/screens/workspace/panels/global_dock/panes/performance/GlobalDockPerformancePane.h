#pragma once

#include "Horo/Editor/Localization/LocalizedGui.h"

#include <array>

struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Performance status story hosted by the global dock. */
    class GlobalDockPerformancePane : public LocalizedGui {
    public:
        /** @brief Catalog scope for this embedded dock surface. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.global_dock.performance"};

        /** @brief Creates a pane with its required localization scope. */
        GlobalDockPerformancePane() : LocalizedGui(kTextScope) {}

        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorGuiContext &context);

    private:
        struct TableLayout;

        void DrawToolbar(const ImVec2 &origin, float width, const EditorGuiContext &context);
        void DrawToolbarSelectors(float &x, float y, const EditorGuiContext &context);
        [[nodiscard]] float DrawMetrics(const ImVec2 &origin, float width, const EditorGuiContext &context) const;
        void DrawTable(const ImVec2 &origin, float width, float height, const EditorGuiContext &context) const;
        void DrawRow(std::size_t index, float width, const TableLayout &layout, const EditorGuiContext &context) const;

        std::array<char, 256> m_search{};
        int m_windowSelection{};
        int m_subsystemSelection{};
        bool m_live{true};
    };
}  // namespace Horo::Editor
