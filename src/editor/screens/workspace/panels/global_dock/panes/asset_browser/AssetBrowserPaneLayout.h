#pragma once

#include "Horo/Editor/EditorTheme.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

namespace Horo::Editor {
    /** @brief Returns the theme-resolved label size used throughout the global dock. */
    [[nodiscard]] inline float GlobalDockLabelFontSize() {
        return Theme::TextPx::Label();
    }

    namespace AssetBrowserLayout {
        inline constexpr float ToolbarHeight = GlobalDockLayout::ToolbarHeight;
        inline constexpr float ToolbarPaddingX = GlobalDockLayout::ToolbarPaddingX;
        inline constexpr float ToolbarControlHeight = GlobalDockLayout::ControlHeight;
        inline constexpr float ToolbarGap = GlobalDockLayout::ToolbarGap;
        inline constexpr float LocationRailWidth = 42.0F;
        inline constexpr float GridPaddingX = 12.0F;
        inline constexpr float GridPaddingTop = 12.0F;
        inline constexpr float GridPaddingBottom = 20.0F;
        inline constexpr float GridGap = 8.0F;
        inline constexpr float CardMinimumWidth = 118.0F;
        inline constexpr float CardMaximumWidth = 152.0F;
        inline constexpr float CardHeight = 120.0F;
        inline constexpr float CardPreviewHeight = 62.0F;

        /** @brief Returns the theme-resolved supporting-text size. */
        [[nodiscard]] inline float SecondaryFontSize() {
            return Theme::TextPx::Caption();
        }
    }  // namespace AssetBrowserLayout

    /** @brief Responsive grid metrics matching the workspace HTML asset-grid contract. */
    struct AssetBrowserGridMetrics {
        std::size_t columns{1};
        float cardWidth{1.0F};
    };

    /**
     * @brief Computes CSS-like `auto-fill minmax(118px, 152px)` asset-card geometry.
     * @param availableWidth Width available to the grid after outer padding.
     * @return Column count and card width using the canonical eight-pixel gap.
     */
    [[nodiscard]] AssetBrowserGridMetrics ComputeAssetBrowserGridMetrics(float availableWidth) noexcept;
}  // namespace Horo::Editor
