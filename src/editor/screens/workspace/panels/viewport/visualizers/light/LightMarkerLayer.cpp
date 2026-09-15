#include "LightMarkerLayer.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/EditorViewportScene.h"

#include <cmath>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Ui::UiIcon IconFor(const Render::RenderLightKind kind) noexcept {
            using enum Render::RenderLightKind;
            using enum Ui::UiIcon;
            switch (kind) {
                case Directional:
                    return DirectionalLight;
                case Point:
                    return PointLight;
                case Spot:
                    return SpotLight;
            }
            return PointLight;
        }

        [[nodiscard]] float DistanceSquared(const ImVec2 left, const ImVec2 right) noexcept {
            const float x = left.x - right.x;
            const float y = left.y - right.y;
            return x * x + y * y;
        }

        [[nodiscard]] Result<std::optional<ImVec2>> ProjectMarkerCenter(const ViewportLightMarkerContext &context,
                                                                        const ViewportLightPresentation &presentation) noexcept {
            const Result<std::optional<EditorViewportPointProjection>> projected =
                ProjectEditorViewportPoint(context.camera, presentation.light.position, context.width / context.height, context.depthRange);
            if (projected.HasError())
                return Result<std::optional<ImVec2>>::Failure(projected.ErrorValue());
            if (!projected.Value().has_value())
                return Result<std::optional<ImVec2>>::Success(std::nullopt);
            return Result<std::optional<ImVec2>>::Success(
                ImVec2{context.origin.x + projected.Value()->viewportPosition.x * context.width,
                       context.origin.y + projected.Value()->viewportPosition.y * context.height});
        }

        void DrawMarker(ImDrawList &drawList, const ViewportLightPresentation &presentation, const ImVec2 center, const bool selected) {
            constexpr float markerSize = 22.0F;
            const ImU32 background = Theme::U32(selected ? Theme::AccentSoft() : Theme::Bg2());
            const ImU32 border = Theme::U32(selected ? Theme::Accent() : Theme::BorderStrong());
            drawList.AddCircleFilled(center, 13.0F, background, 24);
            drawList.AddCircle(center, 13.0F, border, 24, selected ? 2.0F : 1.2F);
            Ui::DrawEditorIcon(&drawList, IconFor(presentation.light.kind), {center.x - markerSize * 0.5F, center.y - markerSize * 0.5F},
                               {markerSize, markerSize}, Theme::U32(Theme::Text()));
        }

        void UpdateClickedMarker(const ViewportLightMarkerContext &context, const ViewportLightPresentation &presentation,
                                 const ImVec2 center, const ImVec2 pointer, float &closestHit,
                                 std::optional<SceneObjectId> &clicked) noexcept {
            if (!context.acceptInput || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                return;
            const float distance = DistanceSquared(pointer, center);
            if (distance > closestHit)
                return;
            closestHit = distance;
            clicked = presentation.object;
        }
    }  // namespace

    Result<std::optional<SceneObjectId>> DrawViewportLightMarkers(const ViewportLightMarkerContext &context,
                                                                  const std::span<const ViewportLightPresentation> lights,
                                                                  const std::optional<SceneObjectId> primarySelection) {
        ImDrawList &drawList = context.drawList;
        if (!std::isfinite(context.width) || !std::isfinite(context.height) || context.width <= 0.0F || context.height <= 0.0F) {
            return Result<std::optional<SceneObjectId>>::Failure(
                MakeError(RendererErrors::InvalidCoordinates, "Viewport Light marker extent must be positive and finite."));
        }
        constexpr float hitRadius = 14.0F;
        const ImVec2 pointer = ImGui::GetMousePos();
        std::optional<SceneObjectId> clicked;
        float closestHit = hitRadius * hitRadius;

        for (const ViewportLightPresentation &presentation : lights) {
            const Result<std::optional<ImVec2>> center = ProjectMarkerCenter(context, presentation);
            if (center.HasError())
                return Result<std::optional<SceneObjectId>>::Failure(center.ErrorValue());
            if (!center.Value().has_value())
                continue;
            const bool selected = primarySelection == presentation.object;
            DrawMarker(drawList, presentation, *center.Value(), selected);
            UpdateClickedMarker(context, presentation, *center.Value(), pointer, closestHit, clicked);
        }
        return Result<std::optional<SceneObjectId>>::Success(clicked);
    }
}  // namespace Horo::Editor
