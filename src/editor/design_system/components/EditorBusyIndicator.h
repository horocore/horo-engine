#pragma once

#include "Horo/Editor/EditorTheme.h"

#include <cmath>
#include <numbers>

namespace Horo::Editor::Ui {
    /** @brief Built-in animated status resource; obeys the host's reduced-motion policy. */
    inline constexpr const char *BusyStatusIcon = "horo.status.busy";

    inline void DrawBusyIndicator(ImDrawList &drawList, const ImVec2 center, const float radius, const ImVec4 color) {
        const float angle =
            Theme::GetActiveTokens().motion.reduceMotion ? 0.0F : static_cast<float>(std::fmod(ImGui::GetTime() * 4, 2 * std::numbers::pi));
        drawList.AddCircle(center, radius, Theme::U32(Theme::BorderStrong()), 20, 1.5F);
        drawList.PathClear();
        drawList.PathArcTo(center, radius, angle, angle + std::numbers::pi_v<float> * 1.4F, 18);
        drawList.PathStroke(Theme::U32(color), 0, 1.8F);
    }
}  // namespace Horo::Editor::Ui
