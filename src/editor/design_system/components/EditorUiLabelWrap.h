#pragma once

#include "Horo/Editor/DesignSystem/DesignTokens.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Editor::Ui {
    inline constexpr float CheckboxLabelGap = 8.0F;

    /** @brief Wraps the visible label at glyph boundaries while preserving the full localized text. */
    inline std::string WrapVisibleLabel(std::string_view label, const float width) {
        auto &font = *ImGui::GetFont();
        const float scale = ImGui::GetFontSize() / font.FontSize;
        const char *remaining = label.data();
        const char *end = remaining + label.size();
        std::string wrapped;
        while (remaining < end) {
            const char *lineEnd = font.CalcWordWrapPositionA(scale, remaining, end, width);
            if (lineEnd == remaining) {
                unsigned int codepoint{};
                lineEnd += std::max(1, ImTextCharFromUtf8(&codepoint, remaining, end));
            }
            wrapped.append(remaining, lineEnd);
            remaining = lineEnd;
            while (remaining < end && *remaining == ' ')
                ++remaining;
            if (remaining < end)
                wrapped.push_back('\n');
        }
        return wrapped;
    }

    struct WrappedButtonLabel final {
        std::string label;
        float width{};
        float height{};
    };

    /** @brief Measures a button with the active font scope and keeps its ID stable across wrapping. */
    inline WrappedButtonLabel MeasureWrappedButtonLabel(const std::string &label, std::string_view id, const float available,
                                                        const DesignSystem::ComponentSizeMetrics &metrics,
                                                        const float minimumWidth = 0.0F) {
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        const float horizontalPadding = metrics.paddingX * 2.0F;
        const float width = std::min(available, std::max(minimumWidth, labelWidth + horizontalPadding));
        const float textWidth = std::max(1.0F, width - horizontalPadding);
        auto visible = labelWidth > textWidth ? WrapVisibleLabel(label, textWidth) : label;
        const float height = std::max(metrics.minimumHeight, ImGui::CalcTextSize(visible.c_str()).y + metrics.paddingY * 2.0F);
        visible.append("###").append(id);
        return {std::move(visible), width, height};
    }
}  // namespace Horo::Editor::Ui
