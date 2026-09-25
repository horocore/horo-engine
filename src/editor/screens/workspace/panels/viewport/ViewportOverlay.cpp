#include "ViewportOverlay.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr float ControlHeight = 36.0F;
        constexpr float ControlGap = 8.0F;
        constexpr float ToolGap = 8.0F;
        constexpr float TopInset = 16.0F;
        constexpr float SideInset = 16.0F;
        constexpr float ButtonWidth = 38.0F;

        enum class OverlayGlyph {
            Select,
            Move,
            Rotate,
            Scale,
            Focus,
            Grid
        };

        /** @brief Draws the four directional arrows of the move tool. */
        void DrawMoveGlyph(ImDrawList &drawList, const ImVec2 center, const ImU32 color, const float stroke) {
            drawList.AddLine({center.x - 8.0F, center.y}, {center.x + 8.0F, center.y}, color, stroke);
            drawList.AddLine({center.x, center.y - 8.0F}, {center.x, center.y + 8.0F}, color, stroke);
            drawList.AddTriangleFilled({center.x + 10.0F, center.y}, {center.x + 5.0F, center.y - 3.0F}, {center.x + 5.0F, center.y + 3.0F},
                                       color);
            drawList.AddTriangleFilled({center.x - 10.0F, center.y}, {center.x - 5.0F, center.y - 3.0F}, {center.x - 5.0F, center.y + 3.0F},
                                       color);
            drawList.AddTriangleFilled({center.x, center.y - 10.0F}, {center.x - 3.0F, center.y - 5.0F}, {center.x + 3.0F, center.y - 5.0F},
                                       color);
            drawList.AddTriangleFilled({center.x, center.y + 10.0F}, {center.x - 3.0F, center.y + 5.0F}, {center.x + 3.0F, center.y + 5.0F},
                                       color);
            drawList.AddCircleFilled(center, 1.7F, color);
        }

        /** @brief Draws the curved arrow of the rotate tool. */
        void DrawRotateGlyph(ImDrawList &drawList, const ImVec2 center, const ImU32 color, const float stroke) {
            constexpr float start = -0.60F;
            constexpr float end = 4.20F;
            drawList.PathArcTo(center, 8.0F, start, end, 24);
            drawList.PathStroke(color, 0, stroke);
            const ImVec2 tip{center.x + 8.0F * std::cos(end), center.y + 8.0F * std::sin(end)};
            const ImVec2 tangent{-std::sin(end), std::cos(end)};
            const ImVec2 radial{std::cos(end), std::sin(end)};
            const ImVec2 base{tip.x - tangent.x * 4.5F, tip.y - tangent.y * 4.5F};
            drawList.AddLine(tip, {base.x + radial.x * 2.6F, base.y + radial.y * 2.6F}, color, stroke);
            drawList.AddLine(tip, {base.x - radial.x * 2.6F, base.y - radial.y * 2.6F}, color, stroke);
        }

        void DrawGlyph(ImDrawList &drawList, const OverlayGlyph glyph, const ImVec2 center, const ImU32 color) {
            constexpr float stroke = 1.8F;
            switch (glyph) {
                case OverlayGlyph::Select: {
                    const std::array points{ImVec2{center.x - 7.0F, center.y - 9.0F}, ImVec2{center.x + 7.0F, center.y - 1.0F},
                                            ImVec2{center.x + 1.0F, center.y + 1.0F}, ImVec2{center.x + 4.0F, center.y + 8.0F},
                                            ImVec2{center.x, center.y + 9.0F},        ImVec2{center.x - 3.0F, center.y + 2.0F},
                                            ImVec2{center.x - 7.0F, center.y + 6.0F}};
                    drawList.AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
                    break;
                }
                case OverlayGlyph::Move:
                    DrawMoveGlyph(drawList, center, color, stroke);
                    break;
                case OverlayGlyph::Rotate:
                    DrawRotateGlyph(drawList, center, color, stroke);
                    break;
                case OverlayGlyph::Scale:
                    drawList.AddRect({center.x - 7.0F, center.y - 1.0F}, {center.x + 1.0F, center.y + 7.0F}, color, 0.0F, 0, stroke);
                    drawList.AddLine({center.x - 1.0F, center.y + 1.0F}, {center.x + 7.0F, center.y - 7.0F}, color, stroke);
                    drawList.AddLine({center.x + 2.0F, center.y - 8.0F}, {center.x + 8.0F, center.y - 8.0F}, color, stroke);
                    drawList.AddLine({center.x + 8.0F, center.y - 8.0F}, {center.x + 8.0F, center.y - 2.0F}, color, stroke);
                    break;
                case OverlayGlyph::Focus:
                    for (const float x : {-1.0F, 1.0F}) {
                        for (const float y : {-1.0F, 1.0F}) {
                            const ImVec2 corner{center.x + x * 8.0F, center.y + y * 8.0F};
                            drawList.AddLine(corner, {corner.x - x * 5.0F, corner.y}, color, stroke);
                            drawList.AddLine(corner, {corner.x, corner.y - y * 5.0F}, color, stroke);
                        }
                    }
                    drawList.AddRect({center.x - 2.5F, center.y - 2.5F}, {center.x + 2.5F, center.y + 2.5F}, color, 0.0F, 0, 1.3F);
                    break;
                case OverlayGlyph::Grid:
                    for (int x = 0; x < 2; ++x)
                        for (int y = 0; y < 2; ++y)
                            drawList.AddRect({center.x - 7.0F + x * 8.0F, center.y - 7.0F + y * 8.0F},
                                             {center.x - 2.0F + x * 8.0F, center.y - 2.0F + y * 8.0F}, color, 0.0F, 0, 1.4F);
                    break;
            }
        }

        [[nodiscard]] bool DrawOverlayButton(const char *id, const ImVec2 position, const OverlayGlyph glyph, const bool selected,
                                             const bool enabled, const char *tooltip, const Theme::Fonts &fonts) {
            ImDrawList &drawList = *ImGui::GetWindowDrawList();
            const ImVec2 end{position.x + ButtonWidth, position.y + ControlHeight};
            ImGui::SetCursorScreenPos(position);
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::InvisibleButton(id, {ButtonWidth, ControlHeight});
            const bool hovered = ImGui::IsItemHovered();
            ImGui::EndDisabled();
            const ImVec4 surface = selected             ? Theme::Mix(Theme::Bg2(), Theme::Accent(), 0.20F)
                                   : hovered && enabled ? Theme::Hover()
                                                        : Theme::Bg2();
            drawList.AddRectFilled(position, end, Theme::U32(surface), 5.0F);
            drawList.AddRect(position, end, Theme::U32(selected ? Theme::Accent() : Theme::Border()), 5.0F);
            DrawGlyph(drawList, glyph, {position.x + ButtonWidth * 0.5F, position.y + ControlHeight * 0.5F},
                      Theme::U32(enabled ? selected ? Theme::Accent() : Theme::Text() : Theme::Muted()));
            if (hovered && tooltip != nullptr)
                Ui::ShowTooltip(tooltip, &fonts);
            return clicked && enabled;
        }

        struct CompassEndpoint {
            ImVec2 tip;
            float depth;
            std::size_t axis;
            bool positive;
        };

        constexpr std::array CompassColors{IM_COL32(239, 92, 100, 255), IM_COL32(135, 213, 92, 255), IM_COL32(79, 157, 244, 255)};
        constexpr std::array CompassMutedColors{IM_COL32(80, 49, 54, 210), IM_COL32(55, 76, 49, 210), IM_COL32(49, 66, 88, 210)};
        constexpr std::array CompassLabels{'X', 'Y', 'Z'};

        /** @brief Draws axis endpoint markers in their depth-sorted order. */
        void DrawCompassMarkers(ImDrawList &drawList, const std::array<CompassEndpoint, 6> &endpoints) {
            for (const CompassEndpoint &endpoint : endpoints) {
                const bool front = endpoint.depth >= 0.0F;
                if (!endpoint.positive) {
                    drawList.AddCircleFilled(endpoint.tip, front ? 6.0F : 5.0F,
                                             front ? CompassColors[endpoint.axis] : CompassMutedColors[endpoint.axis], 16);
                    continue;
                }
                drawList.AddCircleFilled(endpoint.tip, front ? 10.0F : 8.5F,
                                         front ? CompassColors[endpoint.axis] : CompassMutedColors[endpoint.axis], 24);
                const char label[]{CompassLabels[endpoint.axis], '\0'};
                const ImVec2 textSize = ImGui::CalcTextSize(label);
                drawList.AddText({endpoint.tip.x - textSize.x * 0.5F, endpoint.tip.y - textSize.y * 0.5F},
                                 front ? IM_COL32(17, 23, 29, 255) : IM_COL32(174, 184, 196, 255), label);
            }
        }

        /** @brief Resolves a click against the frontmost axis endpoint under the pointer. */
        [[nodiscard]] std::optional<EditorViewportAxisView> HitTestCompass(const std::array<CompassEndpoint, 6> &endpoints,
                                                                           const ImVec2 pointer) {
            for (auto it = endpoints.rbegin(); it != endpoints.rend(); ++it) {
                const bool front = it->depth >= 0.0F;
                const float radius = it->positive ? (front ? 10.0F : 8.5F) : (front ? 6.0F : 5.0F);
                const float dx = pointer.x - it->tip.x;
                const float dy = pointer.y - it->tip.y;
                if (dx * dx + dy * dy > radius * radius)
                    continue;
                constexpr std::array views{
                    EditorViewportAxisView::NegativeX, EditorViewportAxisView::PositiveX, EditorViewportAxisView::NegativeY,
                    EditorViewportAxisView::PositiveY, EditorViewportAxisView::NegativeZ, EditorViewportAxisView::PositiveZ,
                };
                return views[it->axis * 2 + (it->positive ? 1U : 0U)];
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<EditorViewportAxisView> DrawCompass(ImDrawList &drawList, const ImVec2 origin,
                                                                        const EditorViewportCamera &camera) {
            const auto forward = Math::TryNormalize(camera.target - camera.position);
            if (!forward.HasValue())
                return std::nullopt;
            const auto right = Math::TryNormalize(Math::Cross(forward.Value(), camera.up));
            if (!right.HasValue())
                return std::nullopt;
            const Math::Vec3 up = Math::Cross(right.Value(), forward.Value());
            const Math::Vec3 towardCamera = forward.Value() * -1.0F;
            constexpr std::array axes{Math::Vec3{1.0F, 0.0F, 0.0F}, Math::Vec3{0.0F, 1.0F, 0.0F}, Math::Vec3{0.0F, 0.0F, 1.0F}};
            const ImVec2 center{origin.x, origin.y};

            std::array<CompassEndpoint, 6> endpoints{};
            std::size_t endpointCount = 0;
            for (std::size_t i = 0; i < axes.size(); ++i) {
                for (const float sign : {-1.0F, 1.0F}) {
                    const Math::Vec3 axis = axes[i] * sign;
                    endpoints[endpointCount++] = {
                        .tip = {center.x + Math::Dot(axis, right.Value()) * 29.0F, center.y - Math::Dot(axis, up) * 29.0F},
                        .depth = Math::Dot(axis, towardCamera),
                        .axis = i,
                        .positive = sign > 0.0F,
                    };
                }
            }
            std::sort(endpoints.begin(), endpoints.end(), [](const CompassEndpoint &lhs, const CompassEndpoint &rhs) {
                return lhs.depth < rhs.depth;
            });
            for (const CompassEndpoint &endpoint : endpoints) {
                const bool front = endpoint.depth >= 0.0F;
                drawList.AddLine(center, endpoint.tip, front ? CompassColors[endpoint.axis] : CompassMutedColors[endpoint.axis],
                                 front ? 2.0F : 1.5F);
            }
            drawList.AddCircleFilled(center, 4.0F, Theme::U32(Theme::Bg2()), 16);
            drawList.AddCircle(center, 4.0F, Theme::U32(Theme::Border()), 16, 1.0F);
            DrawCompassMarkers(drawList, endpoints);
            ImGui::SetCursorScreenPos({center.x - 40.0F, center.y - 40.0F});
            if (!ImGui::InvisibleButton("##Compass", {80.0F, 80.0F}))
                return std::nullopt;
            return HitTestCompass(endpoints, ImGui::GetMousePos());
        }

        void DrawStats(const ImVec2 origin, const ImVec2 size, const ViewportOverlayState &state,
                       const ILocalizationService &localization) {
            const std::string objects =
                std::vformat(localization.Get("editor", "workspace.viewport.object_count"), std::make_format_args(state.objectCount));
            std::string text = objects;
            if (state.vertexCount)
                text += std::format("  |  {}: {}", localization.Get("editor", "workspace.viewport.vertices"), *state.vertexCount);
            if (state.triangleCount)
                text += std::format("  |  {}: {}", localization.Get("editor", "workspace.viewport.triangles"), *state.triangleCount);
            const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
            const ImVec2 min{origin.x + SideInset, origin.y + size.y - textSize.y - 24.0F};
            ImDrawList &drawList = *ImGui::GetWindowDrawList();
            drawList.AddRectFilled({min.x - 9.0F, min.y - 6.0F}, {min.x + textSize.x + 9.0F, min.y + textSize.y + 6.0F},
                                   Theme::U32(Theme::Bg1()), 5.0F);
            drawList.AddRect({min.x - 9.0F, min.y - 6.0F}, {min.x + textSize.x + 9.0F, min.y + textSize.y + 6.0F},
                             Theme::U32(Theme::Border()), 5.0F);
            drawList.AddText(min, Theme::U32(Theme::Muted()), text.c_str());
        }

        /** @brief Draws viewport transform tools and reports the selected action. */
        void DrawViewportTools(const ImVec2 origin, const ImVec2 size, const float gridX, const ViewportOverlayState &state,
                               const Theme::Fonts &fonts, const ILocalizationService &localization, ViewportOverlayAction &action) {
            const float groupWidth = ButtonWidth * 5.0F + ToolGap * 4.0F;
            float toolX = gridX + ButtonWidth + ToolGap;
            float toolY = origin.y + TopInset;
            if (toolX + groupWidth > origin.x + size.x - SideInset) {
                toolX = origin.x + SideInset;
                toolY += ControlHeight + 8.0F;
            }
            const std::array tools{EditorTransformTool::Select, EditorTransformTool::Move, EditorTransformTool::Rotate,
                                   EditorTransformTool::Scale};
            const std::array glyphs{OverlayGlyph::Select, OverlayGlyph::Move, OverlayGlyph::Rotate, OverlayGlyph::Scale};
            const std::array keys{"workspace.viewport.tool.select", "workspace.viewport.tool.move", "workspace.viewport.tool.rotate",
                                  "workspace.viewport.tool.scale"};
            for (std::size_t i = 0; i < tools.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (DrawOverlayButton("##Tool", {toolX, toolY}, glyphs[i], state.tool == tools[i], true,
                                      localization.Get("editor", keys[i]).c_str(), fonts))
                    action.tool = tools[i];
                ImGui::PopID();
                toolX += ButtonWidth + ToolGap;
            }
            if (DrawOverlayButton("##Focus", {toolX, toolY}, OverlayGlyph::Focus, false, state.canFocusSelection,
                                  localization.Get("editor", "workspace.viewport.focus").c_str(), fonts))
                action.focusSelection = true;
        }
    }  // namespace

    /** @copydoc DrawViewportOverlay */
    ViewportOverlayAction DrawViewportOverlay(const ImVec2 origin, const ImVec2 size, const ViewportOverlayState &state,
                                              const Theme::Fonts &fonts, const ILocalizationService &localization) {
        ViewportOverlayAction action;
        if (size.x <= 0.0F || size.y <= 0.0F)
            return action;

        ImGui::PushID("ViewportOverlay");
        const ImVec2 controlPosition{origin.x + SideInset, origin.y + TopInset};
        const std::array projections{localization.Get("editor", "workspace.viewport.perspective").c_str(),
                                     localization.Get("editor", "workspace.viewport.orthographic").c_str()};
        int projection = state.camera.projection == Runtime::CameraProjection::Perspective ? 0 : 1;
        ImGui::SetCursorScreenPos(controlPosition);
        ImGui::PushItemWidth(140.0F);
        if (Ui::ComboControl("##Projection", &projection, projections.data(), static_cast<int>(projections.size()), fonts,
                             {.height = ControlHeight}))
            action.projection = projection == 0 ? Runtime::CameraProjection::Perspective : Runtime::CameraProjection::Orthographic;
        ImGui::PopItemWidth();

        const char *shaded = localization.Get("editor", "workspace.viewport.shaded").c_str();
        int shading = 0;
        ImGui::SetCursorScreenPos({controlPosition.x + 140.0F + ControlGap, controlPosition.y});
        ImGui::PushItemWidth(116.0F);
        (void)Ui::ComboControl("##Shading", &shading, &shaded, 1, fonts, {.height = ControlHeight});
        ImGui::PopItemWidth();

        const float gridX = controlPosition.x + 140.0F + ControlGap + 116.0F + ControlGap;
        if (DrawOverlayButton("##Grid", {gridX, controlPosition.y}, OverlayGlyph::Grid, state.gridVisible, true,
                              localization.Get("editor", "workspace.viewport.grid").c_str(), fonts))
            action.toggleGrid = true;

        DrawViewportTools(origin, size, gridX, state, fonts, localization, action);

        if (size.x >= 740.0F)
            action.axisView = DrawCompass(*ImGui::GetWindowDrawList(), {origin.x + size.x - 48.0F, origin.y + 64.0F}, state.camera);
        DrawStats(origin, size, state, localization);
        ImGui::PopID();
        return action;
    }
}  // namespace Horo::Editor
