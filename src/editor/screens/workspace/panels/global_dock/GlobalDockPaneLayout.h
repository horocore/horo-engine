#pragma once

#include "Horo/Editor/EditorTheme.h"

#include <algorithm>
#include <imgui.h>

namespace Horo::Editor {
    namespace GlobalDockLayout {
        inline constexpr float ToolbarHeight = 46.0F;
        inline constexpr float ControlHeight = 30.0F;
        inline constexpr float ToolbarGap = 7.0F;
        inline constexpr float ToolbarPaddingX = 10.0F;
        inline constexpr float TableHeaderHeight = 30.0F;
        inline constexpr float TableRowHeight = 34.0F;
        inline constexpr float ContentPadding = 12.0F;
        inline constexpr float ColumnGap = 9.0F;
    }  // namespace GlobalDockLayout

    /** @brief Canonical geometry shared by every pane hosted in the global bottom dock. */
    struct GlobalDockPaneMetrics {
        float toolbarHeight;
        float controlHeight;
        float toolbarGap;
        float toolbarPaddingX;
        float tableHeaderHeight;
        float tableRowHeight;
        float contentPadding;
        float columnGap;
    };

    /** @brief Optional toolbar and rail regions supported by the shared bottom-dock layout contract. */
    struct GlobalDockPaneLayoutOptions {
        bool hasToolbar{true};
        float leftRailWidth{};
    };

    /** @brief Screen-space regions computed for one bottom-dock pane. */
    struct GlobalDockPaneRegions {
        ImVec2 toolbarOrigin{};
        ImVec2 contentOrigin{};
        ImVec2 leftRailOrigin{};
        float toolbarWidth{};
        float contentWidth{};
        float contentHeight{};
        float leftRailHeight{};
    };

    /** @brief Returns the scale-resolved HTML-compatible bottom-dock metrics. */
    [[nodiscard]] inline GlobalDockPaneMetrics ResolveGlobalDockPaneMetrics() noexcept {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        return {
            .toolbarHeight = GlobalDockLayout::ToolbarHeight * scale,
            .controlHeight = GlobalDockLayout::ControlHeight * scale,
            .toolbarGap = GlobalDockLayout::ToolbarGap * scale,
            .toolbarPaddingX = GlobalDockLayout::ToolbarPaddingX * scale,
            .tableHeaderHeight = GlobalDockLayout::TableHeaderHeight * scale,
            .tableRowHeight = GlobalDockLayout::TableRowHeight * scale,
            .contentPadding = GlobalDockLayout::ContentPadding * scale,
            .columnGap = GlobalDockLayout::ColumnGap * scale,
        };
    }

    /** @brief Partitions a pane into toolbar, optional rail, and content regions. */
    [[nodiscard]] inline GlobalDockPaneRegions ResolveGlobalDockPaneRegions(const ImVec2 origin, const float width, const float height,
                                                                            const GlobalDockPaneLayoutOptions options = {}) noexcept {
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float toolbarHeight = options.hasToolbar ? metrics.toolbarHeight : 0.0F;
        const float bodyHeight = std::max(1.0F, height - toolbarHeight);
        const float railWidth = std::clamp(options.leftRailWidth, 0.0F, std::max(0.0F, width - 1.0F));
        return {
            .toolbarOrigin = origin,
            .contentOrigin = {origin.x + railWidth, origin.y + toolbarHeight},
            .leftRailOrigin = {origin.x, origin.y + toolbarHeight},
            .toolbarWidth = width,
            .contentWidth = std::max(1.0F, width - railWidth),
            .contentHeight = bodyHeight,
            .leftRailHeight = bodyHeight,
        };
    }
}  // namespace Horo::Editor
