#include "TransformGizmoGeometry.h"

#include "Horo/Editor/EditorTheme.h"
#include "TransformGizmoMath.h"
#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Editor {
    namespace {
        constexpr float LinearAxisMaximumViewAlignment = 0.95F;
        constexpr float GizmoExtentPixels = 96.0F;
        constexpr float MoveArrowHeadLength = 18.0F;
        constexpr float MoveArrowHeadHalfWidth = 9.0F;
        constexpr float ScaleHandleHalfSize = 9.0F;
        constexpr float ScaleHubHalfSize = 10.0F;
        constexpr float EndOnAxisRadius = 18.0F;
        constexpr int RotationRingSegments = 64;

        /** @brief Camera-aligned frame shared by rotation rings and the active marker. */
        struct RotationScreenBasis {
            Math::Vec3 right;
            Math::Vec3 up;
        };

        [[nodiscard]] Result<RotationScreenBasis> ResolveRotationScreenBasis(const EditorViewportCamera &camera) noexcept {
            const Result<Math::Vec3> forward = Math::TryNormalize(camera.target - camera.position);
            if (forward.HasError())
                return Result<RotationScreenBasis>::Failure(forward.ErrorValue());
            const Result<Math::Vec3> right = Math::TryNormalize(Math::Cross(forward.Value(), camera.up));
            if (right.HasError())
                return Result<RotationScreenBasis>::Failure(right.ErrorValue());
            const Result<Math::Vec3> up = Math::TryNormalize(Math::Cross(right.Value(), forward.Value()));
            if (up.HasError())
                return Result<RotationScreenBasis>::Failure(up.ErrorValue());
            return Result<RotationScreenBasis>::Success({right.Value(), up.Value()});
        }

        [[nodiscard]] ImVec2 ProjectRotationRadial(const ImVec2 center, const Math::Vec3 radial,
                                                   const RotationScreenBasis &screenBasis) noexcept {
            return {
                center.x + GizmoExtentPixels * Math::Dot(radial, screenBasis.right),
                center.y - GizmoExtentPixels * Math::Dot(radial, screenBasis.up),
            };
        }

        [[nodiscard]] std::array<ImU32, 3> RotationAxisColors() noexcept {
            return {
                ImGui::GetColorU32(ImVec4{0.98F, 0.42F, 0.38F, 1.0F}),
                ImGui::GetColorU32(ImVec4{0.45F, 0.88F, 0.49F, 1.0F}),
                ImGui::GetColorU32(ImVec4{0.48F, 0.64F, 1.0F, 1.0F}),
            };
        }

        [[nodiscard]] ImU32 ShadeAxisColor(const ImU32 color, const float brightness) noexcept {
            ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
            rgba.x = std::clamp(rgba.x * brightness, 0.0F, 1.0F);
            rgba.y = std::clamp(rgba.y * brightness, 0.0F, 1.0F);
            rgba.z = std::clamp(rgba.z * brightness, 0.0F, 1.0F);
            return ImGui::ColorConvertFloat4ToU32(rgba);
        }

        [[nodiscard]] ImVec2 AlongAxis(const ImVec2 center, const ImVec2 direction, const float distance,
                                       const float lateral = 0.0F) noexcept {
            return {center.x + direction.x * distance - direction.y * lateral, center.y + direction.y * distance + direction.x * lateral};
        }

        void DrawMoveAxisArrow(ImDrawList &drawList, const ImVec2 center, const ImVec2 direction, const float length, const ImU32 color,
                               const bool highlighted) {
            const ImU32 face = ShadeAxisColor(color, highlighted ? 1.3F : 1.0F);
            const ImU32 light = ShadeAxisColor(face, 1.18F);
            const ImU32 dark = ShadeAxisColor(face, 0.62F);
            const float headBase = length - MoveArrowHeadLength;
            const ImVec2 shaftStart = AlongAxis(center, direction, 7.0F);
            const ImVec2 shaftEnd = AlongAxis(center, direction, headBase);
            const ImVec2 tip = AlongAxis(center, direction, length);
            const ImVec2 left = AlongAxis(center, direction, headBase, -MoveArrowHeadHalfWidth);
            const ImVec2 right = AlongAxis(center, direction, headBase, MoveArrowHeadHalfWidth);
            const ImVec2 middle = AlongAxis(center, direction, headBase);

            drawList.AddLine(shaftStart, shaftEnd, dark, 8.0F);
            drawList.AddLine(AlongAxis(center, direction, 7.0F, -1.0F), AlongAxis(center, direction, headBase, -1.0F), face, 5.0F);
            drawList.AddLine(AlongAxis(center, direction, 9.0F, -2.0F), AlongAxis(center, direction, headBase - 2.0F, -2.0F), light, 1.0F);
            drawList.AddCircleFilled(shaftEnd, 5.0F, dark, 12);
            drawList.AddTriangleFilled(left, tip, middle, light);
            drawList.AddTriangleFilled(middle, tip, right, dark);
        }

        /** @brief Draws a broad scale shaft and a shaded cube handle at the shared gizmo extent. */
        void DrawScaleAxisHandle(ImDrawList &drawList, const ImVec2 center, const ImVec2 direction, const float length, const ImU32 color,
                                 const bool highlighted) {
            const ImU32 face = ShadeAxisColor(color, highlighted ? 1.25F : 1.0F);
            const ImU32 light = ShadeAxisColor(face, 1.18F);
            const ImU32 dark = ShadeAxisColor(face, 0.58F);
            const ImVec2 handle = AlongAxis(center, direction, length - ScaleHandleHalfSize);
            const float shaftLength = length - ScaleHandleHalfSize * 2.0F;
            const ImVec2 shaftEnd = AlongAxis(center, direction, shaftLength);
            drawList.AddLine(AlongAxis(center, direction, 9.0F), shaftEnd, dark, 8.0F);
            drawList.AddLine(AlongAxis(center, direction, 9.0F, -1.0F), AlongAxis(center, direction, shaftLength - 1.0F, -1.0F), face,
                             5.0F);
            const ImVec2 frontMin{handle.x - ScaleHandleHalfSize, handle.y - ScaleHandleHalfSize};
            const ImVec2 frontMax{handle.x + ScaleHandleHalfSize, handle.y + ScaleHandleHalfSize};
            constexpr ImVec2 depth{-3.0F, -3.0F};
            drawList.AddRectFilled({frontMin.x + depth.x, frontMin.y + depth.y}, {frontMax.x + depth.x, frontMax.y + depth.y}, dark, 2.0F);
            drawList.AddQuadFilled({frontMin.x + depth.x, frontMin.y + depth.y}, {frontMax.x + depth.x, frontMax.y + depth.y},
                                   {frontMax.x, frontMin.y}, frontMin, light);
            drawList.AddQuadFilled({frontMax.x + depth.x, frontMin.y + depth.y}, {frontMax.x + depth.x, frontMax.y + depth.y}, frontMax,
                                   {frontMax.x, frontMin.y}, dark);
            drawList.AddRectFilled(frontMin, frontMax, face, 2.0F);
            drawList.AddRect(frontMin, frontMax, dark, 2.0F, 0, 1.5F);
        }

        /** @brief Gives uniform scaling the same solid-handle treatment as the axis tips. */
        void DrawScaleHub(ImDrawList &drawList, const ImVec2 center, const bool highlighted) {
            const ImU32 face = highlighted ? Theme::U32(Theme::Text()) : ImGui::GetColorU32(ImVec4{0.78F, 0.81F, 0.86F, 1.0F});
            const ImVec2 minimum{center.x - ScaleHubHalfSize, center.y - ScaleHubHalfSize};
            const ImVec2 maximum{center.x + ScaleHubHalfSize, center.y + ScaleHubHalfSize};
            drawList.AddRectFilled(minimum, maximum, Theme::U32(Theme::Bg0()), 2.0F);
            drawList.AddRectFilled({minimum.x + 2.0F, minimum.y + 2.0F}, {maximum.x - 2.0F, maximum.y - 2.0F}, face, 2.0F);
            drawList.AddRect(minimum, maximum, Theme::U32(Theme::BorderStrong()), 2.0F, 0, 1.5F);
        }

        void DrawEndOnAxisHandle(ImDrawList &drawList, const ImVec2 center, const ImU32 color, const bool highlighted) {
            drawList.AddCircle(center, EndOnAxisRadius, Theme::U32(Theme::Bg0()), 24, 7.0F);
            drawList.AddCircle(center, EndOnAxisRadius, ShadeAxisColor(color, highlighted ? 1.3F : 1.0F), 24, 4.0F);
        }

        [[nodiscard]] Result<std::optional<ImVec2>> ProjectToViewport(const EditorViewportCamera &camera, const Math::Vec3 worldPosition,
                                                                      const ImVec2 origin, const float width, const float height,
                                                                      const Math::ClipDepthRange depthRange) noexcept {
            if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0F || height <= 0.0F)
                return Result<std::optional<ImVec2>>::Failure(
                    MakeError(TransformGizmoErrors::InvalidRequest, "Transform gizmo viewport extent must be positive and finite."));
            const Result<std::optional<EditorViewportPointProjection>> projected =
                ProjectEditorViewportPoint(camera, worldPosition, width / height, depthRange);
            if (projected.HasError())
                return Result<std::optional<ImVec2>>::Failure(projected.ErrorValue());
            if (!projected.Value().has_value())
                return Result<std::optional<ImVec2>>::Success(std::nullopt);
            const Result<Math::Vec2> pixels = MapEditorViewportPointToPixels(*projected.Value(), {origin.x, origin.y}, {width, height});
            if (pixels.HasError())
                return Result<std::optional<ImVec2>>::Failure(pixels.ErrorValue());
            return Result<std::optional<ImVec2>>::Success(ImVec2{pixels.Value().x, pixels.Value().y});
        }

        [[nodiscard]] float DistanceToSegment(const ImVec2 point, const ImVec2 start, const ImVec2 end) noexcept {
            const ImVec2 segment{end.x - start.x, end.y - start.y};
            const ImVec2 relative{point.x - start.x, point.y - start.y};
            const float lengthSquared = segment.x * segment.x + segment.y * segment.y;
            const float parameter =
                lengthSquared > 0.0F ? std::clamp((relative.x * segment.x + relative.y * segment.y) / lengthSquared, 0.0F, 1.0F) : 0.0F;
            const ImVec2 nearest{start.x + segment.x * parameter, start.y + segment.y * parameter};
            const float deltaX = point.x - nearest.x;
            const float deltaY = point.y - nearest.y;
            return std::sqrt(deltaX * deltaX + deltaY * deltaY);
        }

        [[nodiscard]] float Distance(const ImVec2 lhs, const ImVec2 rhs) noexcept {
            const float x = lhs.x - rhs.x;
            const float y = lhs.y - rhs.y;
            return std::sqrt(x * x + y * y);
        }

        /** @brief Evaluates the shared screen scale at the gizmo pivot, without off-axis perspective shear. */
        [[nodiscard]] Result<float> GizmoPixelsPerWorldUnitAtPivot(const TransformGizmoGeometryRequest &request,
                                                                   const Math::Vec3 worldPosition) noexcept {
            const Result<Math::Vec3> viewDirection = Math::TryNormalize(request.camera.target - request.camera.position);
            if (viewDirection.HasError())
                return Result<float>::Failure(viewDirection.ErrorValue());
            const float depth = Math::Dot(worldPosition - request.camera.position, viewDirection.Value());
            const float visibleWorldHeight = request.camera.projection == Runtime::CameraProjection::Perspective
                                                 ? 2.0F * depth * std::tan(request.camera.verticalFovRadians * 0.5F)
                                                 : request.camera.orthographicHeight;
            const float pixelsPerWorldUnit = request.height / visibleWorldHeight;
            if (!std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 0.0F)
                return Result<float>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Gizmo screen scale is invalid."));
            return Result<float>::Success(pixelsPerWorldUnit);
        }

        void PrepareEndOnAxis(TransformGizmoFrameGeometry &geometry, const int axis, const float pixelsPerWorldUnit,
                              std::optional<int> &endOnAxis) noexcept {
            geometry.screenDirections[axis] = {0.0F, -1.0F};
            geometry.pixelsPerWorldUnit[axis] = std::max(pixelsPerWorldUnit, 1.0F);
            endOnAxis = axis;
        }

        /** @brief Projected ring samples and closest pointer distance for one rotation axis. */
        struct RotationRingProjection {
            std::array<ImVec2, RotationRingSegments + 1> points{};
            float pointerDistance{std::numeric_limits<float>::max()};
        };

        /** @brief Draws one complete projected ring layer without obscuring joins between segments. */
        void DrawRotationRingLayer(ImDrawList &drawList, const RotationRingProjection &ring, const ImU32 color, const float thickness) {
            for (int segment = 1; segment <= RotationRingSegments; ++segment)
                drawList.AddLine(ring.points[segment - 1], ring.points[segment], color, thickness);
        }

        /** @brief Hit-tests the same screen-space samples used to draw a rotation ring. */
        void MeasureRotationRingPointerDistance(RotationRingProjection &ring, const ImVec2 pointer) noexcept {
            for (int segment = 1; segment <= RotationRingSegments; ++segment)
                ring.pointerDistance =
                    std::min(ring.pointerDistance, DistanceToSegment(pointer, ring.points[segment - 1], ring.points[segment]));
        }

        [[nodiscard]] Result<void> DrawRotationHandles(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                                       TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &axisColors) {
            const Result<float> pixelsPerWorldUnit = GizmoPixelsPerWorldUnitAtPivot(request, geometry.worldPosition);
            if (pixelsPerWorldUnit.HasError())
                return Result<void>::Failure(pixelsPerWorldUnit.ErrorValue());
            const Result<RotationScreenBasis> screenBasis = ResolveRotationScreenBasis(request.camera);
            if (screenBasis.HasError())
                return Result<void>::Failure(screenBasis.ErrorValue());

            std::array<RotationRingProjection, 3> rings;
            for (int axis = 0; axis < 3; ++axis) {
                const Math::Vec3 basisU = geometry.worldAxes[(axis + 1) % 3];
                const Result<Math::Vec3> basisV = Math::TryNormalize(Math::Cross(geometry.worldAxes[axis], basisU));
                if (basisV.HasError())
                    return Result<void>::Failure(basisV.ErrorValue());
                geometry.pixelsPerWorldUnit[axis] = pixelsPerWorldUnit.Value();
                for (int segment = 0; segment <= RotationRingSegments; ++segment) {
                    const float angle = 2.0F * std::numbers::pi_v<float> * static_cast<float>(segment) / RotationRingSegments;
                    const Math::Vec3 radial = basisU * std::cos(angle) + basisV.Value() * std::sin(angle);
                    rings[axis].points[segment] = ProjectRotationRadial(*geometry.center, radial, screenBasis.Value());
                }
                MeasureRotationRingPointerDistance(rings[axis], request.pointer);
            }
            if (request.hovered) {
                float closestDistance = 8.0F;
                for (int axis = 0; axis < 3; ++axis) {
                    if (rings[axis].pointerDistance >= closestDistance)
                        continue;
                    closestDistance = rings[axis].pointerDistance;
                    geometry.hoveredAxis = axis;
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                const bool highlighted = request.activeAxis == axis || geometry.hoveredAxis == axis;
                const ImU32 color = highlighted ? ShadeAxisColor(axisColors[axis], 1.15F) : axisColors[axis];
                const float thickness = highlighted ? 5.5F : 4.0F;
                DrawRotationRingLayer(drawList, rings[axis], Theme::U32(Theme::Bg0()), highlighted ? 8.0F : 6.5F);
                DrawRotationRingLayer(drawList, rings[axis], color, thickness);
            }
            return Result<void>::Success();
        }

        /** @brief Shared hit-test state accumulated while drawing linear gizmo handles. */
        struct LinearHandleHitState {
            float closestDistance{std::numeric_limits<float>::max()};
            std::optional<int> endOnAxis;
        };

        struct LinearAxisHandleContext {
            int axis{};
            ImU32 axisColor{};
            const RotationScreenBasis &screenBasis;
            float centerPixelsPerWorldUnit{};
            LinearHandleHitState &hitState;
        };

        [[nodiscard]] Result<void> DrawLinearAxisHandle(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                                        TransformGizmoFrameGeometry &geometry, const LinearAxisHandleContext &context) {
            const int axis = context.axis;
            const ImU32 axisColor = context.axisColor;
            const RotationScreenBasis &screenBasis = context.screenBasis;
            const float centerPixelsPerWorldUnit = context.centerPixelsPerWorldUnit;
            LinearHandleHitState &hitState = context.hitState;
            const Result<bool> directional = HasTransformGizmoLinearAxisScreenDirection(request.camera, geometry.worldAxes[axis]);
            if (directional.HasError())
                return Result<void>::Failure(directional.ErrorValue());
            if (!directional.Value()) {
                PrepareEndOnAxis(geometry, axis, centerPixelsPerWorldUnit, hitState.endOnAxis);
                return Result<void>::Success();
            }
            // Project only the axis orientation: perspective displacement of the pivot must not shear the gizmo.
            const ImVec2 projectedAxis{Math::Dot(geometry.worldAxes[axis], screenBasis.right),
                                       -Math::Dot(geometry.worldAxes[axis], screenBasis.up)};
            const float projectedLength = std::hypot(projectedAxis.x, projectedAxis.y);
            if (!std::isfinite(projectedLength) || projectedLength < 0.001F) {
                PrepareEndOnAxis(geometry, axis, centerPixelsPerWorldUnit, hitState.endOnAxis);
                return Result<void>::Success();
            }
            geometry.pixelsPerWorldUnit[axis] = projectedLength * centerPixelsPerWorldUnit;
            geometry.screenDirections[axis] = {projectedAxis.x / projectedLength, projectedAxis.y / projectedLength};
            const float axisLength = GizmoExtentPixels * std::min(projectedLength, 1.0F);
            const bool move = request.tool == EditorTransformTool::Move;
            const ImVec2 end = AlongAxis(*geometry.center, geometry.screenDirections[axis], axisLength);
            const ImVec2 hitStart = AlongAxis(*geometry.center, geometry.screenDirections[axis], 12.0F);
            const ImVec2 scaleHandle = AlongAxis(*geometry.center, geometry.screenDirections[axis], axisLength - ScaleHandleHalfSize);
            const ImVec2 hitEnd =
                move ? end : AlongAxis(*geometry.center, geometry.screenDirections[axis], axisLength - ScaleHandleHalfSize * 2.0F);
            const float distance = DistanceToSegment(request.pointer, hitStart, hitEnd);
            const bool overScaleHandle = !move && std::fabs(request.pointer.x - scaleHandle.x) <= ScaleHandleHalfSize + 3.0F &&
                                         std::fabs(request.pointer.y - scaleHandle.y) <= ScaleHandleHalfSize + 3.0F;
            const bool active = request.activeAxis == axis;
            const bool hit = request.hovered && (distance <= 10.0F || overScaleHandle);
            if (move)
                DrawMoveAxisArrow(drawList, *geometry.center, geometry.screenDirections[axis], axisLength, axisColor, active || hit);
            else
                DrawScaleAxisHandle(drawList, *geometry.center, geometry.screenDirections[axis], axisLength, axisColor, active || hit);
            // A visible cube handle wins over another axis shaft crossing beneath it.
            if (const float hitDistance = overScaleHandle ? -1.0F : distance; hit && hitDistance < hitState.closestDistance) {
                hitState.closestDistance = hitDistance;
                geometry.hoveredAxis = axis;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> DrawLinearHandles(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                                     TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &axisColors) {
            const Result<RotationScreenBasis> screenBasis = ResolveRotationScreenBasis(request.camera);
            if (screenBasis.HasError())
                return Result<void>::Failure(screenBasis.ErrorValue());
            const Result<float> centerPixelsPerWorldUnit = GizmoPixelsPerWorldUnitAtPivot(request, geometry.worldPosition);
            if (centerPixelsPerWorldUnit.HasError())
                return Result<void>::Failure(centerPixelsPerWorldUnit.ErrorValue());
            LinearHandleHitState hitState;
            for (int axis = 0; axis < 3; ++axis) {
                const Result<void> drawn = DrawLinearAxisHandle(drawList, request, geometry,
                                                                LinearAxisHandleContext{
                                                                    .axis = axis,
                                                                    .axisColor = axisColors[axis],
                                                                    .screenBasis = screenBasis.Value(),
                                                                    .centerPixelsPerWorldUnit = centerPixelsPerWorldUnit.Value(),
                                                                    .hitState = hitState,
                                                                });
                if (drawn.HasError())
                    return drawn;
            }
            if (hitState.endOnAxis.has_value()) {
                const float ringDistance = Distance(request.pointer, *geometry.center);
                const bool hit = request.hovered && std::fabs(ringDistance - EndOnAxisRadius) <= 4.0F;
                DrawEndOnAxisHandle(drawList, *geometry.center, axisColors[*hitState.endOnAxis],
                                    request.activeAxis == hitState.endOnAxis || hit);
                if (hit)
                    geometry.hoveredAxis = hitState.endOnAxis;
            }
            if (request.tool == EditorTransformTool::Scale) {
                const bool uniformHit = request.hovered && std::fabs(request.pointer.x - geometry.center->x) <= ScaleHubHalfSize &&
                                        std::fabs(request.pointer.y - geometry.center->y) <= ScaleHubHalfSize;
                DrawScaleHub(drawList, *geometry.center, uniformHit || request.activeAxis == 3);
                if (uniformHit)
                    geometry.hoveredAxis = 3;
            } else {
                drawList.AddCircleFilled(*geometry.center, 7.0F, Theme::U32(Theme::BorderStrong()), 20);
                drawList.AddCircleFilled(*geometry.center, 5.5F, Theme::U32(Theme::Text()), 20);
                drawList.AddCircleFilled({geometry.center->x - 1.5F, geometry.center->y - 1.5F}, 1.5F, Theme::U32(Theme::Bg0()), 8);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc DrawTransformGizmoGeometry */
    Result<TransformGizmoFrameGeometry> DrawTransformGizmoGeometry(ImDrawList &drawList, const TransformGizmoGeometryRequest &request) {
        TransformGizmoFrameGeometry geometry;
        if (request.activeWorldPosition.has_value()) {
            geometry.worldPosition = *request.activeWorldPosition;
        } else {
            const Result<Math::Vec3> worldPosition = Math::TryTransformPoint(request.worldTransform, {});
            if (worldPosition.HasError())
                return Result<TransformGizmoFrameGeometry>::Failure(worldPosition.ErrorValue());
            geometry.worldPosition = worldPosition.Value();
        }
        const Result<std::array<Math::Vec3, 3>> worldAxes = ResolveTransformGizmoWorldAxes(request.worldTransform, request.space);
        if (worldAxes.HasError())
            return Result<TransformGizmoFrameGeometry>::Failure(worldAxes.ErrorValue());
        geometry.worldAxes = worldAxes.Value();
        const Result<std::optional<ImVec2>> center =
            ProjectToViewport(request.camera, geometry.worldPosition, request.origin, request.width, request.height, request.depthRange);
        if (center.HasError())
            return Result<TransformGizmoFrameGeometry>::Failure(center.ErrorValue());
        geometry.center = center.Value();
        if (!geometry.center.has_value())
            return Result<TransformGizmoFrameGeometry>::Success(std::move(geometry));

        const std::array linearAxisColors{
            ImGui::GetColorU32(ImVec4{0.93F, 0.29F, 0.26F, 1.0F}),
            ImGui::GetColorU32(ImVec4{0.42F, 0.83F, 0.29F, 1.0F}),
            ImGui::GetColorU32(ImVec4{0.25F, 0.51F, 0.96F, 1.0F}),
        };
        if (const Result<void> handles = request.tool == EditorTransformTool::Rotate
                                             ? DrawRotationHandles(drawList, request, geometry, RotationAxisColors())
                                             : DrawLinearHandles(drawList, request, geometry, linearAxisColors);
            handles.HasError()) {
            return Result<TransformGizmoFrameGeometry>::Failure(handles.ErrorValue());
        }
        return Result<TransformGizmoFrameGeometry>::Success(std::move(geometry));
    }

    /** @copydoc DrawTransformGizmoRotationPin */
    Result<void> DrawTransformGizmoRotationPin(ImDrawList &drawList, const EditorViewportCamera &camera, const ImVec2 center,
                                               const Math::Vec3 rotationVector, const int axis) {
        if (axis < 0 || axis >= 3 || !std::isfinite(center.x) || !std::isfinite(center.y))
            return Result<void>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Rotation pin position or axis is invalid."));
        const Result<Math::Vec3> radial = Math::TryNormalize(rotationVector);
        if (radial.HasError())
            return Result<void>::Failure(radial.ErrorValue());
        const Result<RotationScreenBasis> screenBasis = ResolveRotationScreenBasis(camera);
        if (screenBasis.HasError())
            return Result<void>::Failure(screenBasis.ErrorValue());

        const ImVec2 point = ProjectRotationRadial(center, radial.Value(), screenBasis.Value());
        const ImU32 color = RotationAxisColors()[axis];
        drawList.AddCircleFilled({point.x + 1.0F, point.y + 1.5F}, 7.5F, Theme::U32(Theme::Bg0()), 20);
        drawList.AddCircleFilled(point, 6.0F, ShadeAxisColor(color, 0.55F), 20);
        drawList.AddCircleFilled(point, 4.5F, color, 20);
        drawList.AddCircleFilled({point.x - 1.25F, point.y - 1.25F}, 1.5F, ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.9F}), 12);
        return Result<void>::Success();
    }

    /** @copydoc HasTransformGizmoLinearAxisScreenDirection */
    Result<bool> HasTransformGizmoLinearAxisScreenDirection(const EditorViewportCamera &camera, const Math::Vec3 worldAxis) noexcept {
        if (!camera.IsValid())
            return Result<bool>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Transform gizmo camera is invalid."));
        const Result<Math::Vec3> normalizedAxis = Math::TryNormalize(worldAxis);
        if (normalizedAxis.HasError())
            return Result<bool>::Failure(normalizedAxis.ErrorValue());
        const Result<Math::Vec3> viewDirection = Math::TryNormalize(camera.target - camera.position);
        if (viewDirection.HasError())
            return Result<bool>::Failure(viewDirection.ErrorValue());
        const float alignment = std::fabs(Math::Dot(normalizedAxis.Value(), viewDirection.Value()));
        return Result<bool>::Success(alignment <= LinearAxisMaximumViewAlignment);
    }

    /** @copydoc ProjectTransformGizmoRotationVector */
    Result<std::optional<Math::Vec3>> ProjectTransformGizmoRotationVector(const TransformGizmoRotationProjectionRequest &request) noexcept {
        if (!std::isfinite(request.width) || !std::isfinite(request.height) || request.width <= 0.0F || request.height <= 0.0F)
            return Result<std::optional<Math::Vec3>>::Failure(
                MakeError(TransformGizmoErrors::InvalidRequest, "Transform gizmo viewport extent must be positive and finite."));
        const Result<Math::Ray> ray = BuildEditorViewportRay(request.camera, (request.pointer.x - request.origin.x) / request.width,
                                                             (request.pointer.y - request.origin.y) / request.height,
                                                             request.width / request.height, request.depthRange);
        if (ray.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(ray.ErrorValue());
        const Result<Math::Plane> plane = Math::TryMakePlane(request.center, request.normal);
        if (plane.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(plane.ErrorValue());
        const Result<std::optional<Math::RayHit>> hit = Math::IntersectRayPlane(ray.Value(), plane.Value());
        if (hit.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(hit.ErrorValue());
        if (!hit.Value().has_value())
            return Result<std::optional<Math::Vec3>>::Success(std::nullopt);
        const Result<Math::Vec3> vector = Math::TryNormalize(hit.Value()->position - request.center);
        if (vector.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(vector.ErrorValue());
        return Result<std::optional<Math::Vec3>>::Success(vector.Value());
    }
}  // namespace Horo::Editor
