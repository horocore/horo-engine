#pragma once

#include "Horo/Editor/EditorStatusBarModel.h"

#include <imgui.h>
#include <optional>
#include <vector>

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Host-owned ImGui renderer for bounded editor status contributions. */
    class EditorStatusBar {
    public:
        static constexpr float Height = 38.0F;

        EditorStatusBar(const EditorGuiContext &context, EditorStatusItemRegistry &registry);

        /**
         * @brief Draws the persistent status bar and returns an optional typed invocation.
         * @param position Bottom-bar screen position supplied by the editor shell.
         * @param size Fixed-height shell extent.
         * @param context Per-frame active-panel visibility context.
         * @param interactionEnabled False while a modal or leave dialog owns input.
         * @return Invoked item/action IDs, or no value when no enabled item was clicked.
         */
        [[nodiscard]] std::optional<EditorStatusItemInvokedEvent> Draw(const ImVec2 &position, const ImVec2 &size,
                                                                       const EditorStatusBarContext &context, bool interactionEnabled);

    private:
        [[nodiscard]] float MeasureItem(const EditorStatusItem &item) const;
        [[nodiscard]] float PlannedWidth(const EditorStatusItem &item) const;
        [[nodiscard]] bool DrawItem(const EditorStatusItem &item, const ImVec2 &position, float width, bool interactionEnabled) const;

        const EditorGuiContext &context_;
        EditorStatusItemRegistry &registry_;
        std::vector<const EditorStatusItem *> visibleItems_;
        std::vector<EditorStatusMeasuredItem> measuredItems_;
        std::vector<EditorStatusMeasuredItem> rankedItems_;
        EditorStatusBarLayout layout_;
    };
}  // namespace Horo::Editor
