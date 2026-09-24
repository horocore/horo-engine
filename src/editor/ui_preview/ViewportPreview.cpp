#include "ViewportPreview.h"

#include "EditorUiPreviewCatalog.h"
#include "Horo/Editor/EditorTheme.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"

#include <algorithm>
#include <array>
#include <imgui.h>

namespace Horo::Editor {
    namespace {
        void DrawMockScene(ImDrawList &drawList, const ImVec2 origin, const ImVec2 size, const bool gridVisible) {
            const ImVec2 end{origin.x + size.x, origin.y + size.y};
            drawList.AddRectFilledMultiColor(origin, end, IM_COL32(19, 23, 29, 255), IM_COL32(20, 25, 31, 255), IM_COL32(37, 37, 36, 255),
                                             IM_COL32(26, 30, 34, 255));
            const float horizon = origin.y + size.y * 0.42F;
            const float vanishingX = origin.x + size.x * 0.57F;
            if (gridVisible) {
                drawList.PushClipRect({origin.x, horizon}, end, true);
                for (int i = -12; i <= 12; ++i) {
                    const float footX = vanishingX + static_cast<float>(i) * size.x * 0.10F;
                    drawList.AddLine({vanishingX, horizon}, {footX, end.y}, IM_COL32(77, 83, 88, 115), 1.0F);
                }
                for (int i = 1; i <= 12; ++i) {
                    const float t = static_cast<float>(i) / 12.0F;
                    const float y = horizon + t * t * (end.y - horizon);
                    drawList.AddLine({origin.x, y}, {end.x, y}, IM_COL32(73, 79, 84, 125), 1.0F);
                }
                drawList.PopClipRect();
            }

            const float cx = origin.x + size.x * 0.28F;
            const float base = origin.y + size.y * 0.79F;
            const float unit = std::min(size.x, size.y) * 0.12F;
            const ImU32 body = IM_COL32(153, 157, 160, 255);
            const ImU32 edge = IM_COL32(70, 76, 81, 255);
            drawList.AddEllipseFilled({cx, base - unit * 4.35F}, {unit * 0.35F, unit * 0.42F}, body);
            drawList.AddEllipse({cx, base - unit * 4.35F}, {unit * 0.35F, unit * 0.42F}, edge);
            drawList.AddQuadFilled({cx - unit * 0.60F, base - unit * 3.78F}, {cx + unit * 0.60F, base - unit * 3.78F},
                                   {cx + unit * 0.43F, base - unit * 2.30F}, {cx - unit * 0.43F, base - unit * 2.30F}, body);
            drawList.AddLine({cx - unit * 0.75F, base - unit * 3.50F}, {cx - unit * 1.15F, base - unit * 2.15F}, body, unit * 0.27F);
            drawList.AddLine({cx + unit * 0.75F, base - unit * 3.50F}, {cx + unit * 1.15F, base - unit * 2.15F}, body, unit * 0.27F);
            drawList.AddLine({cx - unit * 1.15F, base - unit * 2.15F}, {cx - unit * 1.32F, base - unit * 1.50F}, body, unit * 0.20F);
            drawList.AddLine({cx + unit * 1.15F, base - unit * 2.15F}, {cx + unit * 1.32F, base - unit * 1.50F}, body, unit * 0.20F);
            drawList.AddLine({cx - unit * 0.28F, base - unit * 2.25F}, {cx - unit * 0.34F, base - unit * 0.75F}, body, unit * 0.36F);
            drawList.AddLine({cx + unit * 0.28F, base - unit * 2.25F}, {cx + unit * 0.34F, base - unit * 0.75F}, body, unit * 0.36F);
            drawList.AddLine({cx - unit * 0.34F, base - unit * 0.75F}, {cx - unit * 0.42F, base}, body, unit * 0.24F);
            drawList.AddLine({cx + unit * 0.34F, base - unit * 0.75F}, {cx + unit * 0.42F, base}, body, unit * 0.24F);

            const ImVec2 light{origin.x + size.x * 0.70F, origin.y + size.y * 0.48F};
            drawList.AddCircle(light, size.y * 0.25F, IM_COL32(130, 138, 145, 80), 72, 1.0F);
            drawList.AddEllipse(light, {size.x * 0.15F, size.y * 0.025F}, IM_COL32(130, 138, 145, 110), 0.0F, 64, 1.0F);
            drawList.AddLine(light, {light.x, light.y - 92.0F}, IM_COL32(117, 208, 76, 255), 2.0F);
            drawList.AddTriangleFilled({light.x, light.y - 102.0F}, {light.x - 6.0F, light.y - 88.0F}, {light.x + 6.0F, light.y - 88.0F},
                                       IM_COL32(117, 208, 76, 255));
            drawList.AddLine(light, {light.x + 90.0F, light.y}, IM_COL32(236, 82, 89, 255), 2.0F);
            drawList.AddTriangleFilled({light.x + 102.0F, light.y}, {light.x + 87.0F, light.y - 6.0F}, {light.x + 87.0F, light.y + 6.0F},
                                       IM_COL32(236, 82, 89, 255));
            drawList.AddCircleFilled(light, 9.0F, IM_COL32(255, 230, 171, 255));
            drawList.AddCircle(light, 12.0F, IM_COL32(255, 230, 171, 120), 20, 2.0F);
        }
    }  // namespace

    /** @copydoc DrawViewportPreview */
    void DrawViewportPreview(const Theme::Fonts &fonts, const ILocalizationService &localization) {
        const ImGuiViewport *mainViewport = ImGui::GetMainViewport();
        const ImVec2 canvasMin{mainViewport->WorkPos.x + EditorUiPreviewSidebarWidth + 28.0F,
                               mainViewport->WorkPos.y + EditorUiPreviewHeaderHeight + 28.0F};
        const ImVec2 canvasMax{mainViewport->WorkPos.x + mainViewport->WorkSize.x - 28.0F,
                               mainViewport->WorkPos.y + mainViewport->WorkSize.y - 28.0F};
        const ImVec2 extent{canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y};
        if (extent.x < 320.0F || extent.y < 240.0F)
            return;

        ImGui::SetNextWindowPos(canvasMin);
        ImGui::SetNextWindowSize(extent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::Begin("##ViewportPreview", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        static ViewportOverlayState state{.camera = {.position = {5.0F, 3.0F, 8.0F}, .target = {0.0F, 0.0F, 0.0F}},
                                          .tool = EditorTransformTool::Select,
                                          .gridVisible = true,
                                          .canFocusSelection = true,
                                          .objectCount = 2,
                                          .vertexCount = 12432,
                                          .triangleCount = 24864};
        ImDrawList &drawList = *ImGui::GetWindowDrawList();
        DrawMockScene(drawList, origin, size, state.gridVisible);
        drawList.AddRect(origin, {origin.x + size.x, origin.y + size.y}, Theme::U32(Theme::Border()), 4.0F);
        const ViewportOverlayAction action = DrawViewportOverlay(origin, size, state, fonts, localization);
        if (action.projection)
            state.camera.projection = *action.projection;
        if (action.tool)
            state.tool = *action.tool;
        if (action.toggleGrid)
            state.gridVisible = !state.gridVisible;
        ImGui::End();
        ImGui::PopStyleVar();
    }
}  // namespace Horo::Editor
