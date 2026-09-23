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
    namespace {
        void DrawPreviewChrome(ImDrawList *drawList, const ImVec2 top, const ImVec2 size) {
            constexpr float headerHeight = EditorUiPreviewHeaderHeight;
            constexpr float sidebarWidth = EditorUiPreviewSidebarWidth;
            drawList->AddRectFilled(top, {top.x + size.x, top.y + headerHeight}, Theme::U32(Theme::Bg1()));
            drawList->AddLine({top.x, top.y + headerHeight}, {top.x + size.x, top.y + headerHeight}, Theme::U32(Theme::Border()));
            drawList->AddRectFilled({top.x, top.y + headerHeight}, {top.x + sidebarWidth, top.y + size.y}, Theme::U32(Theme::Bg1()));
            drawList->AddLine({top.x + sidebarWidth, top.y + headerHeight}, {top.x + sidebarWidth, top.y + size.y},
                              Theme::U32(Theme::Border()));
        }

        void DrawPreviewHeader(const Theme::Fonts &fonts, const LocalizationService &localization) {
            ImGui::SetCursorPos({24.0f, 17.0f});
            {
                Theme::ScopedTextStyle style(fonts.sansEmphasis, Theme::TextPx::Heading(), Theme::FontPx::SansEmphasis);
                ImGui::TextColored(Theme::Text(), "%s", localization.Get("editor", "ui_preview.title").c_str());
            }
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::SetCursorPosY(23.0f);
            ImGui::TextColored(Theme::Dim(), "%s", localization.Get("editor", "ui_preview.native_canvas").c_str());
        }

        [[nodiscard]] std::optional<std::string_view> DrawPreviewSidebar(const std::string_view scenarioId, const bool modalOpen,
                                                                         const LocalizationService &localization, ImDrawList *drawList) {
            constexpr float sidebarWidth = EditorUiPreviewSidebarWidth;
            constexpr float headerHeight = EditorUiPreviewHeaderHeight;
            ImGui::SetCursorPos({20.0f, headerHeight + 22.0f});
            ImGui::TextColored(Theme::Dim(), "%s", localization.Get("editor", "ui_preview.scenarios").c_str());
            ImGui::SetCursorPos({14.0f, headerHeight + 54.0f});
            std::optional<std::string_view> requestedScenario;
            for (const auto &scenario : EditorUiPreviewScenarios) {
                ImGui::PushID(scenario.id.data(), scenario.id.data() + scenario.id.size());
                if (const auto &label = localization.Get("editor", scenario.titleKey); Ui::Button({.label = label.c_str(),
                                                                                                   .size = {sidebarWidth - 28.0f, 38.0f},
                                                                                                   .variant = Ui::ButtonVariant::Secondary,
                                                                                                   .enabled = !modalOpen}))
                    requestedScenario = scenario.id;
                if (scenario.id == scenarioId)
                    drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Theme::U32(Theme::Accent()), 5.0f);
                ImGui::PopID();
            }
            return requestedScenario;
        }

        void DrawPreviewGrid(ImDrawList *drawList, const ImVec2 canvasMin, const ImVec2 canvasMax) {
            constexpr float gridStep = 24.0f;
            const ImU32 gridColor = ImGui::ColorConvertFloat4ToU32({Theme::Border().x, Theme::Border().y, Theme::Border().z, 0.25f});
            drawList->PushClipRect(canvasMin, canvasMax, true);
            for (std::size_t index = 0; canvasMin.x + static_cast<float>(index) * gridStep < canvasMax.x; ++index) {
                const float x = canvasMin.x + static_cast<float>(index) * gridStep;
                drawList->AddLine({x, canvasMin.y}, {x, canvasMax.y}, gridColor);
            }
            for (std::size_t index = 0; canvasMin.y + static_cast<float>(index) * gridStep < canvasMax.y; ++index) {
                const float y = canvasMin.y + static_cast<float>(index) * gridStep;
                drawList->AddLine({canvasMin.x, y}, {canvasMax.x, y}, gridColor);
            }
            drawList->PopClipRect();
        }
    }  // namespace

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

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        DrawPreviewChrome(drawList, top, size);
        DrawPreviewHeader(fonts, localization);
        const std::optional<std::string_view> requestedScenario = DrawPreviewSidebar(scenarioId, modalOpen, localization, drawList);
        const ImVec2 canvasMin{top.x + EditorUiPreviewSidebarWidth + 1.0f, top.y + EditorUiPreviewHeaderHeight + 1.0f};
        const ImVec2 canvasMax{top.x + size.x, top.y + size.y};
        DrawPreviewGrid(drawList, canvasMin, canvasMax);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return requestedScenario;
    }
}  // namespace Horo::Editor
