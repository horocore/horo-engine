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
        constexpr float MoveExtentPixels = 140.0F;
        constexpr float MoveArrowHeadLength = 22.0F;
        constexpr float MoveArrowHeadHalfWidth = 7.0F;
        constexpr float MovePlaneOffset = 0.14F;
        constexpr float MovePlaneSize = 0.18F;
        constexpr float MoveEndOnHalfSize = 6.0F;
        constexpr float ScaleHandleHalfSize = 9.0F;
        constexpr float ScaleHubHalfSize = 10.0F;
        constexpr float EndOnAxisRadius = 18.0F;
        constexpr int RotationRingSegments = 64;

        /** @brief Camera-aligned frame shared by rotation rings and the active marker. */
        struct RotationScreenBasis {
            Math::Vec3 right;
            Math::Vec3 up;
            Math::Vec3 towardCamera;
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
            return Result<RotationScreenBasis>::Success({right.Value(), up.Value(), forward.Value() * -1.0F});
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
            const ImU32 face = ShadeAxisColor(color, highlighted ? 1.18F : 1.0F);
            const ImU32 light = ShadeAxisColor(face, 1.1F);
            const ImU32 dark = ShadeAxisColor(face, 0.82F);
            const float headLength = std::min(MoveArrowHeadLength, length * 0.45F);
            const float headWidth = std::min(MoveArrowHeadHalfWidth, length * 0.2F);
            const float headBase = length - headLength;
            const ImVec2 shaftStart = AlongAxis(center, direction, std::min(2.0F, headBase * 0.5F));
            const ImVec2 shaftEnd = AlongAxis(center, direction, headBase);
            const ImVec2 tip = AlongAxis(center, direction, length);
            const ImVec2 left = AlongAxis(center, direction, headBase, -headWidth);
            const ImVec2 right = AlongAxis(center, direction, headBase, headWidth);
            const ImVec2 middle = AlongAxis(center, direction, headBase);

            drawList.AddLine(shaftStart, shaftEnd, dark, 3.0F);
            drawList.AddLine(shaftStart, shaftEnd, face, 1.5F);
            // Joined AA fringes on the two acute half-triangles displace the projected tip.
            const auto flags = drawList.Flags;
            drawList.Flags &= ~ImDrawListFlags_AntiAliasedFill;
            drawList.AddTriangleFilled(left, tip, middle, light);
            drawList.AddTriangleFilled(middle, tip, right, dark);
            drawList.Flags = flags;
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

        /** @brief A depth-gesture marker stays on the actual projected axis endpoint, including exact end-on views. */
        void DrawMoveEndOnAxisHandle(ImDrawList &drawList, const ImVec2 anchor, const ImU32 color, const bool highlighted) {
            drawList.AddCircle(anchor, MoveEndOnHalfSize, ShadeAxisColor(color, highlighted ? 1.2F : 1.0F), 20, 2.0F);
            drawList.AddLine({anchor.x, anchor.y - 3.0F}, {anchor.x, anchor.y + 3.0F}, color, 1.0F);
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
            std::array<float, RotationRingSegments + 1> facing{};
            float pointerDistance{std::numeric_limits<float>::max()};
        };

        /** @brief Draws visible front arcs, clipping each crossing at the view plane. */
        void DrawRotationRingLayer(ImDrawList &drawList, const RotationRingProjection &ring, const ImU32 color, const float thickness,
                                   const bool frontOnly) {
            for (int segment = 1; segment <= RotationRingSegments; ++segment) {
                const float startFacing = ring.facing[segment - 1];
                const float endFacing = ring.facing[segment];
                if (frontOnly && startFacing < 0.0F && endFacing < 0.0F)
                    continue;
                ImVec2 start = ring.points[segment - 1];
                ImVec2 end = ring.points[segment];
                if (frontOnly && (startFacing < 0.0F || endFacing < 0.0F)) {
                    const float parameter = startFacing / (startFacing - endFacing);
                    const ImVec2 crossing{start.x + (end.x - start.x) * parameter, start.y + (end.y - start.y) * parameter};
                    if (startFacing < 0.0F)
                        start = crossing;
                    else
                        end = crossing;
                }
                drawList.AddLine(start, end, color, thickness);
            }
        }

        /** @brief Hit-tests the same screen-space samples used to draw a rotation ring. */
        void MeasureRotationRingPointerDistance(RotationRingProjection &ring, const ImVec2 pointer, const bool frontOnly) noexcept {
            for (int segment = 1; segment <= RotationRingSegments; ++segment) {
                const float startFacing = ring.facing[segment - 1];
                const float endFacing = ring.facing[segment];
                if (frontOnly && startFacing < 0.0F && endFacing < 0.0F)
                    continue;
                ImVec2 start = ring.points[segment - 1];
                ImVec2 end = ring.points[segment];
                if (frontOnly && (startFacing < 0.0F || endFacing < 0.0F)) {
                    const float parameter = startFacing / (startFacing - endFacing);
                    const ImVec2 crossing{start.x + (end.x - start.x) * parameter, start.y + (end.y - start.y) * parameter};
                    if (startFacing < 0.0F)
                        start = crossing;
                    else
                        end = crossing;
                }
                ring.pointerDistance = std::min(ring.pointerDistance, DistanceToSegment(pointer, start, end));
            }
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
                    rings[axis].facing[segment] = Math::Dot(radial, screenBasis.Value().towardCamera);
                }
                MeasureRotationRingPointerDistance(rings[axis], request.pointer, !request.activeAxis.has_value());
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
            if (!request.activeAxis.has_value())
                drawList.AddCircle(*geometry.center, GizmoExtentPixels + 5.0F, ImGui::GetColorU32(ImVec4{0.72F, 0.74F, 0.77F, 0.72F}),
                                   RotationRingSegments, 1.0F);
            for (int axis = 0; axis < 3; ++axis) {
                if (request.activeAxis.has_value() && request.activeAxis != axis)
                    continue;
                const bool highlighted = request.activeAxis == axis || geometry.hoveredAxis == axis;
                const ImU32 color = highlighted ? ShadeAxisColor(axisColors[axis], 1.15F) : axisColors[axis];
                const float thickness = highlighted ? 3.0F : 2.5F;
                DrawRotationRingLayer(drawList, rings[axis], color, thickness, !request.activeAxis.has_value());
            }
            drawList.AddCircleFilled(*geometry.center, 2.5F, ImGui::GetColorU32(ImVec4{1.0F, 0.82F, 0.22F, 1.0F}), 8);
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

        [[nodiscard]] bool PointInQuad(const ImVec2 point, const std::array<ImVec2, 4> &corners) noexcept {
            float winding = 0.0F;
            for (std::size_t index = 0; index < corners.size(); ++index) {
                const ImVec2 start = corners[index];
                const ImVec2 end = corners[(index + 1) % corners.size()];
                const float cross = (end.x - start.x) * (point.y - start.y) - (end.y - start.y) * (point.x - start.x);
                if (std::fabs(cross) < 0.001F)
                    continue;
                if (winding != 0.0F && cross * winding < 0.0F)
                    return false;
                winding = cross;
            }
            return true;
        }

        /** @brief One front-facing projected cube face; the same polygon owns drawing and picking. */
        struct ScaleHandleFace {
            std::array<ImVec2, 4> corners;
            float brightness{};
        };

        /** @brief A cube in the gizmo's equal-length 3D frame, projected with the pivot's shared screen scale. */
        struct ScaleHandleCube {
            std::array<ScaleHandleFace, 3> faces{};
            std::size_t count{};
        };

        /** @brief Projects equal-edge corners and only camera-facing cube faces without per-axis screen normalization. */
        [[nodiscard]] ScaleHandleCube ProjectScaleHandle(const TransformGizmoFrameGeometry &geometry, const int axis,
                                                         const RotationScreenBasis &basis) {
            std::array<ImVec2, 8> corners{};
            const auto handleCenter = geometry.worldAxes[axis] * (GizmoExtentPixels - ScaleHandleHalfSize);
            for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                auto point = handleCenter;
                for (int component = 0; component < 3; ++component)
                    point =
                        point + geometry.worldAxes[component] * ((corner & (1U << component)) ? ScaleHandleHalfSize : -ScaleHandleHalfSize);
                corners[corner] = {geometry.center->x + Math::Dot(point, basis.right), geometry.center->y - Math::Dot(point, basis.up)};
            }
            constexpr std::array<std::array<int, 4>, 6> indices{{
                {{0, 4, 6, 2}},
                {{1, 3, 7, 5}},
                {{0, 1, 5, 4}},
                {{2, 6, 7, 3}},
                {{0, 2, 3, 1}},
                {{4, 5, 7, 6}},
            }};
            ScaleHandleCube cube;
            for (std::size_t faceIndex = 0; faceIndex < indices.size(); ++faceIndex) {
                const auto normal = geometry.worldAxes[faceIndex / 2] * (faceIndex % 2 ? 1.0F : -1.0F);
                const float facing = Math::Dot(normal, basis.towardCamera);
                if (facing <= 0.0001F)
                    continue;
                auto &face = cube.faces[cube.count++];
                for (std::size_t corner = 0; corner < face.corners.size(); ++corner)
                    face.corners[corner] = corners[indices[faceIndex][corner]];
                // ImGui's antialiased convex fill requires clockwise screen-space winding.
                float area{};
                for (std::size_t corner = 0; corner < face.corners.size(); ++corner) {
                    const auto &from = face.corners[corner];
                    const auto &to = face.corners[(corner + 1) % face.corners.size()];
                    area += from.x * to.y - from.y * to.x;
                }
                if (area < 0)
                    std::reverse(face.corners.begin(), face.corners.end());
                face.brightness =
                    std::clamp(0.72F + 0.18F * facing + 0.18F * Math::Dot(normal, basis.up) - 0.10F * Math::Dot(normal, basis.right), 0.5F,
                               1.15F);
            }
            return cube;
        }

        /** @brief Draws shaded 3D faces rather than a rotated screen square with a fixed depth offset. */
        void DrawScaleAxisHandle(ImDrawList &drawList, const ImVec2 center, const ImVec2 direction, const float shaftLength,
                                 const ScaleHandleCube &cube, const ImU32 color, const bool highlighted) {
            const ImU32 face = ShadeAxisColor(color, highlighted ? 1.25F : 1.0F);
            const ImU32 dark = ShadeAxisColor(face, 0.58F);
            drawList.AddLine(AlongAxis(center, direction, 9.0F), AlongAxis(center, direction, shaftLength), dark, 8.0F);
            drawList.AddLine(AlongAxis(center, direction, 9.0F, -1.0F), AlongAxis(center, direction, shaftLength, -1.0F), face, 5.0F);
            for (std::size_t index = 0; index < cube.count; ++index) {
                const auto &quad = cube.faces[index];
                // Nearly edge-on faces have acute projected corners. ImGui's
                // joined AA fringe can extend those corners into long spikes.
                const auto flags = drawList.Flags;
                drawList.Flags &= ~ImDrawListFlags_AntiAliasedFill;
                drawList.AddQuadFilled(quad.corners[0], quad.corners[1], quad.corners[2], quad.corners[3],
                                       ShadeAxisColor(face, quad.brightness));
                drawList.Flags = flags;
                for (std::size_t edge = 0; edge < quad.corners.size(); ++edge)
                    drawList.AddLine(quad.corners[edge], quad.corners[(edge + 1) % quad.corners.size()], dark, 1.0F);
            }
        }

        /** @brief Picks visible cube polygons and a bounded edge tolerance, including foreshortened side faces. */
        [[nodiscard]] bool OverScaleHandle(const ImVec2 pointer, const ScaleHandleCube &cube) noexcept {
            for (std::size_t face = 0; face < cube.count; ++face) {
                const auto &corners = cube.faces[face].corners;
                if (PointInQuad(pointer, corners))
                    return true;
                for (std::size_t edge = 0; edge < corners.size(); ++edge)
                    if (DistanceToSegment(pointer, corners[edge], corners[(edge + 1) % corners.size()]) <= 3.0F)
                        return true;
            }
            return false;
        }

        /** @brief One camera authority shared by all Move primitives; no local view/projection convention. */
        struct MoveProjection {
            const TransformGizmoGeometryRequest &request;
            Math::Mat4 viewProjection;
        };

        /** @brief A quad clipped by seven half-spaces has at most eleven corners. */
        struct MoveClipPolygon {
            std::array<Math::Vec4, TransformGizmoPlanePolygon::MaximumCorners> points{};
            std::size_t count{};
        };

        constexpr std::size_t MoveClipPlanes = 7;
        using MovePlaneProjection = Result<std::optional<TransformGizmoPlanePolygon>>;
        using MoveAxisProjection = Result<std::optional<TransformGizmoAxisSegment>>;

        /** @brief Canonical homogeneous clip half-spaces; eye epsilon matches the shared projectability contract. */
        double MoveClipDistance(const Math::Vec4 point, const std::size_t plane, const Math::ClipDepthRange depth) noexcept {
            switch (plane) {
                case 0:
                    return static_cast<double>(point.w) - 2.0 * Math::DefaultEpsilon;
                case 1:
                    return static_cast<double>(point.w) + point.x;
                case 2:
                    return static_cast<double>(point.w) - point.x;
                case 3:
                    return static_cast<double>(point.w) + point.y;
                case 4:
                    return static_cast<double>(point.w) - point.y;
                case 5:
                    return depth == Math::ClipDepthRange::NegativeOneToOne ? static_cast<double>(point.z) + point.w : point.z;
                default:
                    return static_cast<double>(point.w) - point.z;
            }
        }

        /** @brief Interpolates the same projective edge before division, preserving exact endpoints. */
        Math::Vec4 MoveClipLerp(const Math::Vec4 first, const Math::Vec4 last, const double fraction) noexcept {
            if (fraction == 0.0)
                return first;
            if (fraction == 1.0)
                return last;
            return {static_cast<float>(first.x + (static_cast<double>(last.x) - first.x) * fraction),
                    static_cast<float>(first.y + (static_cast<double>(last.y) - first.y) * fraction),
                    static_cast<float>(first.z + (static_cast<double>(last.z) - first.z) * fraction),
                    static_cast<float>(first.w + (static_cast<double>(last.w) - first.w) * fraction)};
        }

        /** @brief Duplicate clip intersections never create zero-length polygon edges or grow fixed scratch. */
        bool AppendMoveClip(MoveClipPolygon &output, const Math::Vec4 point) noexcept {
            if (output.count != 0 && output.points[output.count - 1] == point)
                return true;
            if (output.count == output.points.size())
                return false;
            output.points[output.count++] = point;
            return true;
        }

        /** @brief One bounded convex half-space pass, including exact-on-plane vertices. */
        bool ClipMovePolygonPlane(const MoveClipPolygon &input, MoveClipPolygon &output, const std::size_t plane,
                                  const Math::ClipDepthRange depth) noexcept {
            output.count = 0;
            auto previous = input.points[input.count - 1];
            auto before = MoveClipDistance(previous, plane, depth);
            for (const auto current : std::span{input.points}.first(input.count)) {
                const auto after = MoveClipDistance(current, plane, depth);
                if ((before >= 0.0) != (after >= 0.0))
                    if (!AppendMoveClip(output, MoveClipLerp(previous, current, before / (before - after))))
                        return false;
                if (after >= 0.0 && !AppendMoveClip(output, current))
                    return false;
                previous = current;
                before = after;
            }
            if (output.count > 1 && output.points[0] == output.points[output.count - 1])
                --output.count;
            return true;
        }

        /** @brief Divides only admitted clipped coordinates, then reuses the canonical viewport-to-pixel adapter. */
        Result<ImVec2> MoveClipPixels(const MoveProjection &projection, const Math::Vec4 point) noexcept {
            if (!Math::IsFinite(point) || point.w <= Math::DefaultEpsilon)
                return Result<ImVec2>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Clipped gizmo vertex is not projectable."));
            const EditorViewportPointProjection normalized{.viewportPosition = {point.x / point.w * 0.5F + 0.5F,
                                                                                0.5F - point.y / point.w * 0.5F},
                                                           .ndcDepth = point.z / point.w};
            const auto &request = projection.request;
            const auto pixels =
                MapEditorViewportPointToPixels(normalized, {request.origin.x, request.origin.y}, {request.width, request.height});
            if (pixels.HasError())
                return Result<ImVec2>::Failure(pixels.ErrorValue());
            return Result<ImVec2>::Success({pixels.Value().x, pixels.Value().y});
        }

        /** @brief Uses translated pixel edges for stable area measurement away from the screen origin. */
        double MovePolygonArea(const TransformGizmoPlanePolygon &polygon) noexcept {
            double area = 0.0;
            const auto origin = polygon.corners[0];
            for (std::size_t index = 1; index + 1 < polygon.count; ++index) {
                const auto a = polygon.corners[index];
                const auto b = polygon.corners[index + 1];
                area += (static_cast<double>(a.x) - origin.x) * (static_cast<double>(b.y) - origin.y) -
                        (static_cast<double>(a.y) - origin.y) * (static_cast<double>(b.x) - origin.x);
            }
            return area * 0.5;
        }

        /** @brief Clips first, divides last, and rejects only truly empty or unusably degenerate surviving polygons. */
        MovePlaneProjection ProjectMovePolygon(const MoveProjection &projection, MoveClipPolygon polygon) {
            MoveClipPolygon scratch;
            for (std::size_t plane = 0; plane < MoveClipPlanes; ++plane) {
                if (!ClipMovePolygonPlane(polygon, scratch, plane, projection.request.depthRange) || scratch.count < 3)
                    return MovePlaneProjection::Success(std::nullopt);
                std::swap(polygon, scratch);
            }
            TransformGizmoPlanePolygon result;
            result.count = polygon.count;
            for (std::size_t index = 0; index < polygon.count; ++index) {
                const auto point = MoveClipPixels(projection, polygon.points[index]);
                if (point.HasError())
                    return MovePlaneProjection::Failure(point.ErrorValue());
                result.corners[index] = point.Value();
            }
            if (std::fabs(MovePolygonArea(result)) < 8.0)
                return MovePlaneProjection::Success(std::nullopt);
            return MovePlaneProjection::Success(result);
        }

        /** @brief Projects the actual authored world-plane corners, retaining all visible clip intersections. */
        MovePlaneProjection ProjectMovePlane(const MoveProjection &projection, const TransformGizmoFrameGeometry &geometry,
                                             const int normal, const float worldExtent) {
            const auto first = geometry.worldAxes[(normal + 1) % 3] * worldExtent;
            const auto second = geometry.worldAxes[(normal + 2) % 3] * worldExtent;
            const auto base = first * MovePlaneOffset - second * MovePlaneOffset;
            const std::array offsets{base, base + first * MovePlaneSize, base + (first - second) * MovePlaneSize,
                                     base - second * MovePlaneSize};
            MoveClipPolygon polygon;
            polygon.count = offsets.size();
            for (std::size_t index = 0; index < offsets.size(); ++index) {
                const auto world = geometry.worldPosition + offsets[index];
                polygon.points[index] = Math::TransformHomogeneous(projection.viewProjection, {world.x, world.y, world.z, 1.0F});
                if (!Math::IsFinite(polygon.points[index]))
                    return MovePlaneProjection::Failure(
                        MakeError(TransformGizmoErrors::InvalidRequest, "Gizmo plane overflows clip coordinates."));
            }
            return ProjectMovePolygon(projection, polygon);
        }

        /** @brief Finds the retained parameter interval of the original homogeneous axis segment. */
        bool ClipMoveAxis(const MoveProjection &projection, const Math::Vec4 first, const Math::Vec4 last, double &minimum,
                          double &maximum) noexcept {
            minimum = 0.0;
            maximum = 1.0;
            for (std::size_t plane = 0; plane < MoveClipPlanes; ++plane) {
                const auto a = MoveClipDistance(first, plane, projection.request.depthRange);
                const auto b = MoveClipDistance(last, plane, projection.request.depthRange);
                if (a < 0.0 && b < 0.0)
                    return false;
                if ((a < 0.0) == (b < 0.0))
                    continue;
                const auto crossing = a / (a - b);
                if (a < 0.0)
                    minimum = std::max(minimum, crossing);
                else
                    maximum = std::min(maximum, crossing);
            }
            return minimum <= maximum;
        }

        /** @brief A clipped shaft retains its real world-length fraction for interaction sensitivity. */
        MoveAxisProjection ProjectMoveAxis(const MoveProjection &projection, const TransformGizmoFrameGeometry &geometry, const int axis,
                                           const float worldExtent) {
            const auto world = geometry.worldPosition + geometry.worldAxes[axis] * worldExtent;
            const auto first = Math::TransformHomogeneous(projection.viewProjection, {geometry.worldPosition.x, geometry.worldPosition.y,
                                                                                      geometry.worldPosition.z, 1.0F});
            const auto last = Math::TransformHomogeneous(projection.viewProjection, {world.x, world.y, world.z, 1.0F});
            if (!Math::IsFinite(first) || !Math::IsFinite(last))
                return MoveAxisProjection::Failure(
                    MakeError(TransformGizmoErrors::InvalidRequest, "Gizmo axis overflows clip coordinates."));
            double minimum = 0.0;
            double maximum = 1.0;
            if (!ClipMoveAxis(projection, first, last, minimum, maximum) || minimum == maximum)
                return MoveAxisProjection::Success(std::nullopt);
            const auto start = MoveClipPixels(projection, MoveClipLerp(first, last, minimum));
            const auto end = MoveClipPixels(projection, MoveClipLerp(first, last, maximum));
            if (start.HasError())
                return MoveAxisProjection::Failure(start.ErrorValue());
            if (end.HasError())
                return MoveAxisProjection::Failure(end.ErrorValue());
            return MoveAxisProjection::Success(TransformGizmoAxisSegment{start.Value(), end.Value(), minimum, maximum, maximum == 1.0});
        }

        /** @brief Pointer admission shares the visible viewport instead of picking clipped offscreen geometry. */
        bool MovePointerAvailable(const TransformGizmoGeometryRequest &request) noexcept {
            return request.hovered && std::isfinite(request.pointer.x) && std::isfinite(request.pointer.y) &&
                   request.pointer.x >= request.origin.x && request.pointer.x <= request.origin.x + request.width &&
                   request.pointer.y >= request.origin.y && request.pointer.y <= request.origin.y + request.height;
        }

        /** @brief Convex polygon picking uses exactly the surviving rendered points, independent of winding. */
        bool PointInMovePolygon(const ImVec2 point, const TransformGizmoPlanePolygon &polygon) noexcept {
            double winding = 0.0;
            for (std::size_t index = 0; index < polygon.count; ++index) {
                const auto a = polygon.corners[index];
                const auto b = polygon.corners[(index + 1) % polygon.count];
                const double cross = (static_cast<double>(b.x) - a.x) * (static_cast<double>(point.y) - a.y) -
                                     (static_cast<double>(b.y) - a.y) * (static_cast<double>(point.x) - a.x);
                if (std::fabs(cross) < 0.001F)
                    continue;
                if (winding != 0.0F && cross * winding < 0.0F)
                    return false;
                winding = cross;
            }
            return true;
        }

        /** @brief Uses exactly the same clipped polygon for drawing and plane selection. */
        [[nodiscard]] Result<void> DrawMovePlaneHandles(ImDrawList &drawList, const MoveProjection &projection,
                                                        TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &axisColors,
                                                        const float centerPixelsPerWorldUnit) {
            const auto &request = projection.request;
            for (int normal = 0; normal < 3; ++normal) {
                const auto projected = ProjectMovePlane(projection, geometry, normal, MoveExtentPixels / centerPixelsPerWorldUnit);
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                if (!projected.Value())
                    continue;
                const auto &polygon = *projected.Value();
                geometry.movePlaneCorners[normal] = polygon;
                const bool hit = MovePointerAvailable(request) && PointInMovePolygon(request.pointer, polygon);
                const bool highlighted = request.activeAxis == normal + 4 || hit;
                const ImU32 color = ShadeAxisColor(axisColors[normal], highlighted ? 1.25F : 0.95F);
                const auto flags = drawList.Flags;
                drawList.Flags &= ~ImDrawListFlags_AntiAliasedFill;
                const auto points = polygon.Points();
                drawList.AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
                drawList.Flags = flags;
                drawList.AddPolyline(points.data(), static_cast<int>(points.size()), ShadeAxisColor(color, 0.72F), ImDrawFlags_Closed,
                                     1.0F);
                if (hit)
                    geometry.hoveredAxis = normal + 4;
            }
            return Result<void>::Success();
        }

        /** @brief Clipping a shaft does not manufacture an arrow head at the frustum intersection. */
        void DrawClippedMoveShaft(ImDrawList &drawList, const TransformGizmoAxisSegment &segment, const ImU32 color,
                                  const bool highlighted) {
            const auto face = ShadeAxisColor(color, highlighted ? 1.18F : 1.0F);
            drawList.AddLine(segment.start, segment.end, ShadeAxisColor(face, 0.82F), 3.0F);
            drawList.AddLine(segment.start, segment.end, face, 1.5F);
        }

        /** @brief Stores a real end-on axis with an explicit usable depth gesture at its actual projected anchor. */
        void PrepareMoveDepthAxis(TransformGizmoFrameGeometry &geometry, const int axis, const ImVec2 anchor,
                                  const float centerPixelsPerWorldUnit) noexcept {
            geometry.moveEndOnAnchors[axis] = anchor;
            geometry.moveAxisGestures[axis] = TransformGizmoAxisGesture::ViewDepth;
            // Vertical pointer travel is the authored depth gesture; worldAxes[axis] remains the real normalized selected axis.
            geometry.screenDirections[axis] = {0.0F, -1.0F};
            geometry.pixelsPerWorldUnit[axis] = centerPixelsPerWorldUnit;
        }

        /** @brief Publishes and draws one admitted clipped segment using that identical segment for pointer selection. */
        Result<void> DrawVisibleMoveAxis(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                         TransformGizmoFrameGeometry &geometry, const LinearAxisHandleContext &context,
                                         const TransformGizmoAxisSegment &segment, const float worldExtent, const float length) {
            const int axis = context.axis;
            const ImVec2 direction{(segment.end.x - segment.start.x) / length, (segment.end.y - segment.start.y) / length};
            geometry.screenDirections[axis] = direction;
            const double sensitivity = length / (worldExtent * (segment.lastParameter - segment.firstParameter));
            if (!std::isfinite(sensitivity) || sensitivity <= 0.0 || sensitivity > std::numeric_limits<float>::max())
                return Result<void>::Failure(
                    MakeError(TransformGizmoErrors::InvalidRequest, "Move axis sensitivity is not representable."));
            geometry.pixelsPerWorldUnit[axis] = static_cast<float>(sensitivity);
            geometry.projectedAxisVisible[axis] = true;
            const auto start = AlongAxis(segment.start, direction, std::min(10.0F, length * 0.25F));
            const float distance = DistanceToSegment(request.pointer, start, segment.end);
            const bool hit = MovePointerAvailable(request) && distance <= 8.0F;
            const bool highlighted = request.activeAxis == axis || hit;
            if (segment.tipVisible)
                DrawMoveAxisArrow(drawList, segment.start, direction, length, context.axisColor, highlighted);
            else
                DrawClippedMoveShaft(drawList, segment, context.axisColor, highlighted);
            if (hit && distance < context.hitState.closestDistance) {
                context.hitState.closestDistance = distance;
                geometry.hoveredAxis = axis;
            }
            return Result<void>::Success();
        }

        /** @brief Retains only the real clipped shaft; sensitivity uses its actual retained world fraction. */
        [[nodiscard]] Result<void> DrawMoveAxis(ImDrawList &drawList, const MoveProjection &projection,
                                                TransformGizmoFrameGeometry &geometry, const LinearAxisHandleContext &context) {
            const auto &request = projection.request;
            const int axis = context.axis;
            const float worldExtent = MoveExtentPixels / context.centerPixelsPerWorldUnit;
            const auto projected = ProjectMoveAxis(projection, geometry, axis, worldExtent);
            if (projected.HasError())
                return Result<void>::Failure(projected.ErrorValue());
            if (!projected.Value())
                return Result<void>::Success();
            const auto &segment = *projected.Value();
            geometry.moveAxisSegments[axis] = segment;
            const float length = Distance(segment.start, segment.end);
            if (const double projectedExtent = length / (segment.lastParameter - segment.firstParameter); projectedExtent < 2.0) {
                PrepareMoveDepthAxis(geometry, axis, segment.end, context.centerPixelsPerWorldUnit);
                return Result<void>::Success();
            }
            if (length < 0.001F)
                return Result<void>::Success();
            return DrawVisibleMoveAxis(drawList, request, geometry, context, segment, worldExtent, length);
        }

        /** @brief The same anchored depth marker owns both its drawn circle and bounded pointer region. */
        void DrawMoveEndOnHandle(ImDrawList &drawList, const TransformGizmoGeometryRequest &request, TransformGizmoFrameGeometry &geometry,
                                 const int axis, const ImU32 color) {
            const auto anchor = *geometry.moveEndOnAnchors[axis];
            const bool hit = MovePointerAvailable(request) && Distance(request.pointer, anchor) <= MoveEndOnHalfSize + 2.0F;
            DrawMoveEndOnAxisHandle(drawList, anchor, color, request.activeAxis == axis || hit);
            if (hit)
                geometry.hoveredAxis = axis;
        }

        [[nodiscard]] Result<void> DrawMoveHandles(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                                   TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &colors) {
            const auto scale = GizmoPixelsPerWorldUnitAtPivot(request, geometry.worldPosition);
            if (scale.HasError())
                return Result<void>::Failure(scale.ErrorValue());
            const auto matrix = BuildEditorViewportViewProjection(request.camera, request.width / request.height, request.depthRange);
            if (matrix.HasError())
                return Result<void>::Failure(matrix.ErrorValue());
            const auto basis = ResolveRotationScreenBasis(request.camera);
            if (basis.HasError())
                return Result<void>::Failure(basis.ErrorValue());
            const MoveProjection projection{request, matrix.Value()};
            LinearHandleHitState hitState;
            for (int axis = 0; axis < 3; ++axis) {
                const auto drawn =
                    DrawMoveAxis(drawList, projection, geometry, {axis, colors[axis], basis.Value(), scale.Value(), hitState});
                if (drawn.HasError())
                    return drawn;
            }
            if (const auto planes = DrawMovePlaneHandles(drawList, projection, geometry, colors, scale.Value()); planes.HasError())
                return planes;
            for (int axis = 0; axis < 3; ++axis)
                if (geometry.moveEndOnAnchors[axis])
                    DrawMoveEndOnHandle(drawList, request, geometry, axis, colors[axis]);
            drawList.AddCircleFilled(*geometry.center, 3.5F, Theme::U32(Theme::Bg0()), 12);
            drawList.AddCircleFilled(*geometry.center, 2.0F, Theme::U32(Theme::Text()), 12);
            return Result<void>::Success();
        }

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
            geometry.projectedAxisVisible[axis] = true;
            geometry.screenDirections[axis] = {projectedAxis.x / projectedLength, projectedAxis.y / projectedLength};
            const bool move = request.tool == EditorTransformTool::Move;
            const float axisLength = (move ? MoveExtentPixels : GizmoExtentPixels) * std::min(projectedLength, 1.0F);
            const ImVec2 end = AlongAxis(*geometry.center, geometry.screenDirections[axis], axisLength);
            const ImVec2 hitStart = AlongAxis(*geometry.center, geometry.screenDirections[axis], move ? 10.0F : 12.0F);
            const float scaleShaftLength = (GizmoExtentPixels - ScaleHandleHalfSize * 2.0F) * projectedLength;
            const ImVec2 hitEnd = move ? end : AlongAxis(*geometry.center, geometry.screenDirections[axis], scaleShaftLength);
            const float distance = DistanceToSegment(request.pointer, hitStart, hitEnd);
            const ScaleHandleCube cube = move ? ScaleHandleCube{} : ProjectScaleHandle(geometry, axis, screenBasis);
            const bool overScaleHandle = !move && OverScaleHandle(request.pointer, cube);
            const bool active = request.activeAxis == axis;
            const bool hit = request.hovered && (distance <= (move ? 8.0F : 10.0F) || overScaleHandle);
            if (move)
                DrawMoveAxisArrow(drawList, *geometry.center, geometry.screenDirections[axis], axisLength, axisColor, active || hit);
            else
                DrawScaleAxisHandle(drawList, *geometry.center, geometry.screenDirections[axis], scaleShaftLength, cube, axisColor,
                                    active || hit);
            // A visible cube handle wins over another axis shaft crossing beneath it.
            if (const float hitDistance = overScaleHandle ? -1.0F : distance;
                hit && (hitDistance < hitState.closestDistance || (overScaleHandle && hitDistance == hitState.closestDistance))) {
                hitState.closestDistance = hitDistance;
                geometry.hoveredAxis = axis;
            }
            return Result<void>::Success();
        }

        void DrawScaleEndOnHandle(ImDrawList &drawList, const TransformGizmoGeometryRequest &request, TransformGizmoFrameGeometry &geometry,
                                  const int axis, const ImU32 color) {
            const bool hit = request.hovered && std::fabs(Distance(request.pointer, *geometry.center) - EndOnAxisRadius) <= 4.0F;
            DrawEndOnAxisHandle(drawList, *geometry.center, color, request.activeAxis == axis || hit);
            if (hit)
                geometry.hoveredAxis = axis;
        }

        void DrawScaleCenterHandle(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                   TransformGizmoFrameGeometry &geometry) {
            const bool hit = request.hovered && std::fabs(request.pointer.x - geometry.center->x) <= ScaleHubHalfSize &&
                             std::fabs(request.pointer.y - geometry.center->y) <= ScaleHubHalfSize;
            DrawScaleHub(drawList, *geometry.center, hit || request.activeAxis == 3);
            if (hit)
                geometry.hoveredAxis = 3;
        }

        [[nodiscard]] Result<void> DrawLinearHandles(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                                     TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &axisColors) {
            const auto screenBasis = ResolveRotationScreenBasis(request.camera);
            if (screenBasis.HasError())
                return Result<void>::Failure(screenBasis.ErrorValue());
            const auto scale = GizmoPixelsPerWorldUnitAtPivot(request, geometry.worldPosition);
            if (scale.HasError())
                return Result<void>::Failure(scale.ErrorValue());
            LinearHandleHitState hitState;
            std::array axisOrder{0, 1, 2};
            std::sort(axisOrder.begin(), axisOrder.end(), [&](const int left, const int right) {
                const float leftDepth = Math::Dot(geometry.worldAxes[left], screenBasis.Value().towardCamera);
                const float rightDepth = Math::Dot(geometry.worldAxes[right], screenBasis.Value().towardCamera);
                return leftDepth < rightDepth || (leftDepth == rightDepth && left < right);
            });
            for (const int axis : axisOrder) {
                const auto drawn = DrawLinearAxisHandle(drawList, request, geometry,
                                                        {axis, axisColors[axis], screenBasis.Value(), scale.Value(), hitState});
                if (drawn.HasError())
                    return drawn;
            }
            if (hitState.endOnAxis.has_value())
                DrawScaleEndOnHandle(drawList, request, geometry, *hitState.endOnAxis, axisColors[*hitState.endOnAxis]);
            DrawScaleCenterHandle(drawList, request, geometry);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> DrawHandles(ImDrawList &drawList, const TransformGizmoGeometryRequest &request,
                                               TransformGizmoFrameGeometry &geometry, const std::array<ImU32, 3> &colors) {
            if (request.tool == EditorTransformTool::Move)
                return DrawMoveHandles(drawList, request, geometry, colors);
            if (request.tool == EditorTransformTool::Rotate)
                return DrawRotationHandles(drawList, request, geometry, RotationAxisColors());
            return DrawLinearHandles(drawList, request, geometry, colors);
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
        if (const Result<void> handles = DrawHandles(drawList, request, geometry, linearAxisColors); handles.HasError()) {
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
        drawList.AddCircleFilled(point, 4.0F, Theme::U32(Theme::Bg0()), 12);
        drawList.AddCircleFilled(point, 2.5F, color, 12);
        drawList.AddCircleFilled(point, 1.0F, ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.9F}), 8);
        return Result<void>::Success();
    }

    /** @copydoc DrawTransformGizmoRotationSweep */
    Result<void> DrawTransformGizmoRotationSweep(ImDrawList &drawList, const EditorViewportCamera &camera, const ImVec2 center,
                                                 const Math::Vec3 worldAxis, const Math::Vec3 startVector, const Math::Vec3 currentVector) {
        if (!std::isfinite(center.x) || !std::isfinite(center.y))
            return Result<void>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Rotation sweep center is invalid."));
        const Result<RotationScreenBasis> screenBasis = ResolveRotationScreenBasis(camera);
        if (screenBasis.HasError())
            return Result<void>::Failure(screenBasis.ErrorValue());
        const Result<Math::Vec3> axis = Math::TryNormalize(worldAxis);
        const Result<Math::Vec3> start = Math::TryNormalize(startVector);
        const Result<Math::Vec3> current = Math::TryNormalize(currentVector);
        if (axis.HasError() || start.HasError() || current.HasError())
            return Result<void>::Failure(MakeError(TransformGizmoErrors::InvalidRequest, "Rotation sweep vectors are invalid."));
        const float angle =
            std::atan2(Math::Dot(axis.Value(), Math::Cross(start.Value(), current.Value())), Math::Dot(start.Value(), current.Value()));
        const int segments =
            std::max(1, static_cast<int>(std::ceil(std::fabs(angle) * RotationRingSegments / (2.0F * std::numbers::pi_v<float>))));
        const ImU32 fill = ImGui::GetColorU32(ImVec4{0.82F, 0.84F, 0.88F, 0.22F});
        const ImU32 guide = ImGui::GetColorU32(ImVec4{0.92F, 0.93F, 0.96F, 0.92F});
        const Math::Vec3 tangent = Math::Cross(axis.Value(), start.Value());
        ImVec2 previous = ProjectRotationRadial(center, start.Value(), screenBasis.Value());
        for (int segment = 1; segment <= segments; ++segment) {
            const float stepAngle = angle * static_cast<float>(segment) / static_cast<float>(segments);
            const Math::Vec3 radial = start.Value() * std::cos(stepAngle) + tangent * std::sin(stepAngle);
            const ImVec2 next = ProjectRotationRadial(center, radial, screenBasis.Value());
            drawList.AddTriangleFilled(center, previous, next, fill);
            previous = next;
        }
        drawList.AddLine(center, ProjectRotationRadial(center, start.Value(), screenBasis.Value()), guide, 1.5F);
        drawList.AddLine(center, ProjectRotationRadial(center, current.Value(), screenBasis.Value()), guide, 1.5F);
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

    /** @copydoc ProjectTransformGizmoPlanePoint */
    Result<std::optional<Math::Vec3>> ProjectTransformGizmoPlanePoint(const TransformGizmoPlaneProjectionRequest &request) noexcept {
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
        return Result<std::optional<Math::Vec3>>::Success(hit.Value()->position);
    }

    /** @copydoc ProjectTransformGizmoRotationVector */
    Result<std::optional<Math::Vec3>> ProjectTransformGizmoRotationVector(const TransformGizmoPlaneProjectionRequest &request) noexcept {
        const Result<std::optional<Math::Vec3>> point = ProjectTransformGizmoPlanePoint(request);
        if (point.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(point.ErrorValue());
        if (!point.Value().has_value())
            return Result<std::optional<Math::Vec3>>::Success(std::nullopt);
        const Result<Math::Vec3> vector = Math::TryNormalize(*point.Value() - request.center);
        if (vector.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(vector.ErrorValue());
        return Result<std::optional<Math::Vec3>>::Success(vector.Value());
    }
}  // namespace Horo::Editor
