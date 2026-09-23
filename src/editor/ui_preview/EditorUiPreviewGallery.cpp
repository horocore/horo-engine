/** @file EditorUiPreviewGallery.cpp
 * @brief Isolated native canvas for inspecting editor UI scenarios.
 */

#include "EditorUiPreviewGallery.h"

#include "EditorUiPreviewCatalog.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizationService.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @copydoc DrawEditorUiPreviewGallery */
    std::optional<std::string_view> DrawEditorUiPreviewGallery(const std::string_view scenarioId, const bool modalOpen,
                                                               const Theme::Fonts &fonts, const LocalizationService &localization) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg0());
        ImGui::Begin("##EditorUiPreviewGallery", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        constexpr float headerHeight = EditorUiPreviewHeaderHeight;
        constexpr float sidebarWidth = EditorUiPreviewSidebarWidth;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        drawList->AddRectFilled(top, {top.x + size.x, top.y + headerHeight}, Theme::U32(Theme::Bg1()));
        drawList->AddLine({top.x, top.y + headerHeight}, {top.x + size.x, top.y + headerHeight}, Theme::U32(Theme::Border()));
        drawList->AddRectFilled({top.x, top.y + headerHeight}, {top.x + sidebarWidth, top.y + size.y}, Theme::U32(Theme::Bg1()));
        drawList->AddLine({top.x + sidebarWidth, top.y + headerHeight}, {top.x + sidebarWidth, top.y + size.y},
                          Theme::U32(Theme::Border()));

        ImGui::SetCursorPos({24.0f, 17.0f});
        {
            Theme::ScopedTextStyle style(fonts.sansEmphasis, Theme::TextPx::Heading(), Theme::FontPx::SansEmphasis);
            ImGui::TextColored(Theme::Text(), "%s", localization.Get("editor", "ui_preview.title").c_str());
        }
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::SetCursorPosY(23.0f);
        ImGui::TextColored(Theme::Dim(), "%s", localization.Get("editor", "ui_preview.native_canvas").c_str());

        ImGui::SetCursorPos({20.0f, headerHeight + 22.0f});
        ImGui::TextColored(Theme::Dim(), "%s", localization.Get("editor", "ui_preview.scenarios").c_str());
        ImGui::SetCursorPos({14.0f, headerHeight + 54.0f});
        std::optional<std::string_view> requestedScenario;
        for (const auto &scenario : EditorUiPreviewScenarios) {
            ImGui::PushID(scenario.id.data(), scenario.id.data() + scenario.id.size());
            const char *label = localization.Get("editor", scenario.titleKey).c_str();
            if (Ui::Button({.label = label,
                            .size = {sidebarWidth - 28.0f, 38.0f},
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = !modalOpen}))
                requestedScenario = scenario.id;
            if (scenario.id == scenarioId) {
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                drawList->AddRect(min, max, Theme::U32(Theme::Accent()), 5.0f);
            }
            ImGui::PopID();
        }

        const ImVec2 canvasMin{top.x + sidebarWidth + 1.0f, top.y + headerHeight + 1.0f};
        const ImVec2 canvasMax{top.x + size.x, top.y + size.y};
        constexpr float gridStep = 24.0f;
        const ImU32 gridColor = ImGui::ColorConvertFloat4ToU32({Theme::Border().x, Theme::Border().y, Theme::Border().z, 0.25f});
        drawList->PushClipRect(canvasMin, canvasMax, true);
        for (float x = canvasMin.x; x < canvasMax.x; x += gridStep)
            drawList->AddLine({x, canvasMin.y}, {x, canvasMax.y}, gridColor);
        for (float y = canvasMin.y; y < canvasMax.y; y += gridStep)
            drawList->AddLine({canvasMin.x, y}, {canvasMax.x, y}, gridColor);
        drawList->PopClipRect();

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return requestedScenario;
    }
}  // namespace Horo::Editor
