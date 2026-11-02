#include "editor/renderer/EditorViewportScene.h"
#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoGeometry.h"
#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoMath.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <imgui_internal.h>
#include <limits>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ImGuiGizmoGeometryTestContext {
        ImGuiGizmoGeometryTestContext()
            : request{.camera = camera,
                      .worldTransform = worldTransform,
                      .tool = EditorTransformTool::Move,
                      .space = EditorTransformSpace::World,
                      .width = 400.0F,
                      .height = 400.0F,
                      .pointer = {265.0F, 200.0F},
                      .hovered = true} {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = {400.0F, 400.0F};
            io.DeltaTime = 1.0F / 60.0F;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
            ImGui::NewFrame();
            ImGui::Begin("GizmoGeometryTest");
        }

        ~ImGuiGizmoGeometryTestContext() {
            ImGui::End();
            ImGui::Render();
            ImGui::DestroyContext();
        }

        [[nodiscard]] ImDrawList &DrawList() const {
            return *ImGui::GetWindowDrawList();
        }

        EditorViewportCamera camera;
        Math::Mat4 worldTransform = Math::Mat4::Identity();
        TransformGizmoGeometryRequest request;
    };

    [[nodiscard]] ImVec2 ProjectMoveOracle(const TransformGizmoGeometryRequest &request, const Math::Vec3 point) {
        const auto projected = ProjectEditorViewportPoint(request.camera, point, request.width / request.height, request.depthRange);
        REQUIRE(projected.HasValue());
        REQUIRE(projected.Value());
        const auto pixels =
            MapEditorViewportPointToPixels(*projected.Value(), {request.origin.x, request.origin.y}, {request.width, request.height});
        REQUIRE(pixels.HasValue());
        return {pixels.Value().x, pixels.Value().y};
    }

    [[nodiscard]] float MoveOracleWorldExtent(const TransformGizmoGeometryRequest &request, const Math::Vec3 pivot) {
        const auto forward = Math::TryNormalize(request.camera.target - request.camera.position);
        REQUIRE(forward.HasValue());
        const float depth = Math::Dot(pivot - request.camera.position, forward.Value());
        const float worldHeight = request.camera.projection == Runtime::CameraProjection::Perspective
                                      ? 2.0F * depth * std::tan(request.camera.verticalFovRadians * 0.5F)
                                      : request.camera.orthographicHeight;
        return 140.0F * worldHeight / request.height;
    }

    void RequireNearbyGizmoPoint(const ImVec2 actual, const ImVec2 expected, const float tolerance = 0.001F) {
        REQUIRE(std::hypot(actual.x - expected.x, actual.y - expected.y) < tolerance);
    }

    /** @brief Independent slab intersection uses real camera-forward world depth, not the implementation's clip-plane loop. */
    [[nodiscard]] Math::Vec3 IntersectMoveDepthOracle(const EditorViewportCamera &camera, const Math::Vec3 first, const Math::Vec3 last,
                                                      const float boundary) {
        const auto forward = Math::TryNormalize(camera.target - camera.position);
        REQUIRE(forward.HasValue());
        const double a = Math::Dot(first - camera.position, forward.Value());
        const double b = Math::Dot(last - camera.position, forward.Value());
        const double fraction = (boundary - a) / (b - a);
        REQUIRE(fraction >= 0.0);
        REQUIRE(fraction <= 1.0);
        return first + (last - first) * static_cast<float>(fraction);
    }

    /** @brief Uses the canonical VP without culling exact float-rounded boundary coordinates. */
    [[nodiscard]] ImVec2 ProjectMoveBoundaryOracle(const TransformGizmoGeometryRequest &request, const Math::Vec3 point) {
        const auto vp = BuildEditorViewportViewProjection(request.camera, request.width / request.height, request.depthRange);
        REQUIRE(vp.HasValue());
        const auto ndc = Math::TryProject(vp.Value(), point);
        REQUIRE(ndc.HasValue());
        const EditorViewportPointProjection projected{.viewportPosition = {ndc.Value().x * 0.5F + 0.5F, 0.5F - ndc.Value().y * 0.5F},
                                                      .ndcDepth = ndc.Value().z};
        const auto pixels =
            MapEditorViewportPointToPixels(projected, {request.origin.x, request.origin.y}, {request.width, request.height});
        REQUIRE(pixels.HasValue());
        return {pixels.Value().x, pixels.Value().y};
    }

    /** @brief Checks exact retained vertices are submitted by the real ImGui polygon fill, not a copied fixture. */
    void RequireMovePolygonDrawn(const ImDrawList &list, const int firstVertex, const TransformGizmoPlanePolygon &polygon) {
        REQUIRE(polygon.count >= 3);
        REQUIRE(polygon.count <= TransformGizmoPlanePolygon::MaximumCorners);
        for (const auto corner : polygon) {
            float nearest = std::numeric_limits<float>::max();
            for (int vertex = firstVertex; vertex < list.VtxBuffer.Size; ++vertex) {
                const auto point = list.VtxBuffer[vertex].pos;
                nearest = std::min(nearest, std::hypot(point.x - corner.x, point.y - corner.y));
            }
            REQUIRE(nearest < 0.01F);
        }
    }

    [[nodiscard]] ImVec2 MovePolygonCenter(const TransformGizmoPlanePolygon &polygon) {
        ImVec2 center{};
        for (const auto point : polygon) {
            center.x += point.x / static_cast<float>(polygon.count);
            center.y += point.y / static_cast<float>(polygon.count);
        }
        return center;
    }

    /** @brief Counts actual ImGui allocator use while forwarding the existing allocator and restoring it on every exit. */
    struct GizmoAllocationProbe {
        ImGuiMemAllocFunc allocate{};
        ImGuiMemFreeFunc free{};
        void *user{};
        std::size_t allocations{};

        GizmoAllocationProbe() {
            ImGui::GetAllocatorFunctions(&allocate, &free, &user);
            ImGui::SetAllocatorFunctions(Allocate, Free, this);
        }

        ~GizmoAllocationProbe() {
            ImGui::SetAllocatorFunctions(allocate, free, user);
        }

        static void *Allocate(const std::size_t size, void *opaque) {
            auto &probe = *static_cast<GizmoAllocationProbe *>(opaque);
            ++probe.allocations;
            return probe.allocate(size, probe.user);
        }

        static void Free(void *pointer, void *opaque) {
            auto &probe = *static_cast<GizmoAllocationProbe *>(opaque);
            probe.free(pointer, probe.user);
        }
    };

    [[nodiscard]] float MeasureTransformGizmoToolExtent(ImDrawList &drawList, const TransformGizmoGeometryRequest &baseRequest,
                                                        const EditorTransformTool tool) {
        TransformGizmoGeometryRequest request = baseRequest;
        request.tool = tool;
        request.hovered = false;
        const int firstVertex = drawList.VtxBuffer.Size;
        const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, request);
        REQUIRE(geometry.HasValue());
        REQUIRE(geometry.Value().center.has_value());
        const ImVec2 center = *geometry.Value().center;
        float extent = 0.0F;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex)
            extent = std::max(extent, std::hypot(drawList.VtxBuffer[vertexIndex].pos.x - center.x,
                                                 drawList.VtxBuffer[vertexIndex].pos.y - center.y));
        return extent;
    }

    [[nodiscard]] std::pair<float, ImVec2> MeasureBlueTransformGizmoScaleHandle(ImDrawList &drawList, const EditorViewportCamera &camera,
                                                                                const Math::Mat4 &worldTransform) {
        const TransformGizmoGeometryRequest request{
            .camera = camera,
            .worldTransform = worldTransform,
            .tool = EditorTransformTool::Scale,
            .space = EditorTransformSpace::World,
            .width = 400.0F,
            .height = 400.0F,
            .pointer = {265.0F, 200.0F},
            .hovered = false,
        };
        const int firstVertex = drawList.VtxBuffer.Size;
        const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, request);
        REQUIRE(geometry.HasValue());
        REQUIRE(geometry.Value().center.has_value());
        float extent = 0.0F;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            const auto color = ImGui::ColorConvertU32ToFloat4(vertex.col);
            if (color.w > 0 && color.z > color.x * 1.5F && color.z > color.y * 1.3F)
                extent = std::max(extent, std::hypot(vertex.pos.x - geometry.Value().center->x, vertex.pos.y - geometry.Value().center->y));
        }
        return std::pair{extent, geometry.Value().screenDirections[2]};
    }

    [[nodiscard]] std::array<ImU32, 3> TransformGizmoRotationColors() {
        return {ImGui::GetColorU32(ImVec4{0.98F, 0.42F, 0.38F, 1.0F}), ImGui::GetColorU32(ImVec4{0.45F, 0.88F, 0.49F, 1.0F}),
                ImGui::GetColorU32(ImVec4{0.48F, 0.64F, 1.0F, 1.0F})};
    }

    [[nodiscard]] std::array<float, 3> MeasureTransformGizmoRotationRadii(ImDrawList &drawList, const int firstVertex,
                                                                          const std::array<ImU32, 3> &colors, const ImVec2 center) {
        std::array<float, 3> radii{};
        for (int index = firstVertex; index < drawList.VtxBuffer.Size; ++index) {
            const ImDrawVert &vertex = drawList.VtxBuffer[index];
            for (int axis = 0; axis < 3; ++axis)
                if (vertex.col == colors[axis])
                    radii[axis] = std::max(radii[axis], std::hypot(vertex.pos.x - center.x, vertex.pos.y - center.y));
        }
        return radii;
    }

    void RequireMatchedTransformGizmoRotationRadii(const std::array<float, 3> &radii) {
        for (const float radius : radii)
            REQUIRE(radius >= 92.0F);
        for (const float radius : radii)
            REQUIRE(radius <= 102.0F);
        REQUIRE(*std::max_element(radii.begin(), radii.end()) - *std::min_element(radii.begin(), radii.end()) < 5.0F);
    }

    void RequireTransformGizmoRotationRingCrossings(ImDrawList &drawList, const int firstVertex, const ImVec2 center,
                                                    const std::array<ImU32, 3> &colors, const Math::Vec3 &right, const Math::Vec3 &up) {
        const std::array<Math::Vec3, 3> worldBasis{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
        for (int sharedAxis = 0; sharedAxis < 3; ++sharedAxis) {
            const ImVec2 crossing{center.x + 96.0F * Math::Dot(worldBasis[sharedAxis], right),
                                  center.y - 96.0F * Math::Dot(worldBasis[sharedAxis], up)};
            for (int ringAxis = 0; ringAxis < 3; ++ringAxis) {
                if (ringAxis == sharedAxis)
                    continue;
                float nearest = std::numeric_limits<float>::max();
                for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
                    const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
                    if (vertex.col == colors[ringAxis])
                        nearest = std::min(nearest, std::hypot(vertex.pos.x - crossing.x, vertex.pos.y - crossing.y));
                }
                REQUIRE(nearest < 4.0F);
            }
        }
    }

    [[nodiscard]] std::array<std::vector<ImVec2>, 3> CollectTransformGizmoRotationRingOffsets(ImDrawList &drawList, const int firstVertex,
                                                                                              const ImVec2 center,
                                                                                              const std::array<ImU32, 3> &colors) {
        std::array<std::vector<ImVec2>, 3> offsets;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            for (int axis = 0; axis < 3; ++axis)
                if (vertex.col == colors[axis])
                    offsets[axis].push_back({vertex.pos.x - center.x, vertex.pos.y - center.y});
        }
        return offsets;
    }

    void RequireTransformGizmoRotationPin(ImDrawList &drawList, const EditorViewportCamera &camera, const ImVec2 center,
                                          const Math::Vec3 radial, const int activeAxis, const std::array<ImU32, 3> &colors,
                                          const Math::Vec3 &right, const Math::Vec3 &up) {
        const int firstVertex = drawList.VtxBuffer.Size;
        const Result<void> pin = DrawTransformGizmoRotationPin(drawList, camera, center, radial, activeAxis);
        REQUIRE(pin.HasValue());
        const ImU32 highlight = ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.9F});
        const ImVec2 expected{center.x + 96.0F * Math::Dot(radial, right), center.y - 96.0F * Math::Dot(radial, up)};
        bool foundAxisColor = false;
        float closestHighlight = std::numeric_limits<float>::max();
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            foundAxisColor = foundAxisColor || vertex.col == colors[activeAxis];
            if (vertex.col == highlight)
                closestHighlight = std::min(closestHighlight, std::hypot(vertex.pos.x - expected.x, vertex.pos.y - expected.y));
        }
        REQUIRE(foundAxisColor);
        REQUIRE(closestHighlight < 2.0F);
    }
}  // namespace

TEST_CASE("Transform gizmo linear and plane handles remain selectable", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();

    const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, context.request);
    REQUIRE(geometry.HasValue());
    REQUIRE(geometry.Value().hoveredAxis == 0);
    REQUIRE(geometry.Value().center.has_value());

    TransformGizmoGeometryRequest hubRequest = context.request;
    hubRequest.pointer = {200.0F, 200.0F};
    const Result<TransformGizmoFrameGeometry> hub = DrawTransformGizmoGeometry(drawList, hubRequest);
    REQUIRE(hub.HasValue());
    REQUIRE(hub.Value().hoveredAxis == 2);
    REQUIRE(hub.Value().moveAxisGestures[2] == TransformGizmoAxisGesture::ViewDepth);

    TransformGizmoGeometryRequest planeRequest = context.request;
    planeRequest.pointer = {222.0F, 222.0F};
    const Result<TransformGizmoFrameGeometry> plane = DrawTransformGizmoGeometry(drawList, planeRequest);
    REQUIRE(plane.HasValue());
    REQUIRE(plane.Value().hoveredAxis == 6);
}

TEST_CASE("Transform gizmo projected handles remain selectable across camera angles", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();

    EditorViewportCamera distantCamera = context.camera;
    distantCamera.position = {0.0F, 0.0F, 8.0F};
    const TransformGizmoGeometryRequest distantRequest{.camera = distantCamera,
                                                       .worldTransform = context.worldTransform,
                                                       .tool = EditorTransformTool::Move,
                                                       .space = EditorTransformSpace::World,
                                                       .width = 400.0F,
                                                       .height = 400.0F,
                                                       .pointer = {222.0F, 222.0F},
                                                       .hovered = true};
    const Result<TransformGizmoFrameGeometry> distantPlane = DrawTransformGizmoGeometry(drawList, distantRequest);
    REQUIRE(distantPlane.HasValue());
    REQUIRE(distantPlane.Value().hoveredAxis == 6);

    TransformGizmoGeometryRequest endOnRequest = context.request;
    endOnRequest.pointer = {200.0F, 200.0F};
    const Result<TransformGizmoFrameGeometry> endOnZ = DrawTransformGizmoGeometry(drawList, endOnRequest);
    REQUIRE(endOnZ.HasValue());
    REQUIRE(endOnZ.Value().hoveredAxis == 2);
    REQUIRE(endOnZ.Value().pixelsPerWorldUnit[2] > 0.0F);
    REQUIRE(endOnZ.Value().screenDirections[2].y == -1.0F);

    context.camera.position = {4.0F, 0.0F, 0.0F};
    TransformGizmoGeometryRequest endOnXRequest{
        .camera = context.camera,
        .worldTransform = context.worldTransform,
        .tool = EditorTransformTool::Move,
        .space = EditorTransformSpace::World,
        .width = 400.0F,
        .height = 400.0F,
        .pointer = endOnRequest.pointer,
        .hovered = endOnRequest.hovered,
    };
    const Result<TransformGizmoFrameGeometry> endOnX = DrawTransformGizmoGeometry(drawList, endOnXRequest);
    REQUIRE(endOnX.HasValue());
    REQUIRE(endOnX.Value().hoveredAxis == 0);

    endOnXRequest.tool = EditorTransformTool::Scale;
    endOnXRequest.pointer = {212.75F, 212.75F};
    const Result<TransformGizmoFrameGeometry> scaleEndOnX = DrawTransformGizmoGeometry(drawList, endOnXRequest);
    REQUIRE(scaleEndOnX.HasValue());
    REQUIRE(scaleEndOnX.Value().hoveredAxis == 0);
}

TEST_CASE("Move plane pointer projection stays on the selected world plane", "[unit][editor][viewport][gizmo]") {
    EditorViewportCamera camera;
    const Result<std::optional<Math::Vec3>> point = ProjectTransformGizmoPlanePoint(
        {.camera = camera, .center = {}, .normal = {0.0F, 0.0F, 1.0F}, .pointer = {220.0F, 180.0F}, .width = 400.0F, .height = 400.0F});
    REQUIRE(point.HasValue());
    REQUIRE(point.Value().has_value());
    REQUIRE(std::fabs(point.Value()->z) < 1e-5F);
    REQUIRE(point.Value()->x > 0.0F);
    REQUIRE(point.Value()->y > 0.0F);
}

TEST_CASE("Move plane handles project fixed world-plane corners across camera angles", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    for (const Math::Vec3 camera : std::array{Math::Vec3{4, 3, 4}, Math::Vec3{-4, 3, 4}}) {
        context.camera.position = camera;
        const auto geometry = DrawTransformGizmoGeometry(context.DrawList(), context.request);
        REQUIRE(geometry.HasValue());
        const auto &frame = geometry.Value();
        const float extent = MoveOracleWorldExtent(context.request, frame.worldPosition);
        for (int normal = 0; normal < 3; ++normal) {
            REQUIRE(frame.movePlaneCorners[normal]);
            const auto first = frame.worldAxes[(normal + 1) % 3] * extent;
            const auto second = frame.worldAxes[(normal + 2) % 3] * extent;
            const auto base = frame.worldPosition + (first - second) * 0.14F;
            const std::array points{base, base + first * 0.18F, base + (first - second) * 0.18F, base - second * 0.18F};
            for (std::size_t corner = 0; corner < points.size(); ++corner)
                RequireNearbyGizmoPoint((*frame.movePlaneCorners[normal])[corner], ProjectMoveOracle(context.request, points[corner]));
        }
    }
}

TEST_CASE("Move plane polygons share front and back facing picking", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    for (const float cameraZ : {4.0F, -4.0F}) {
        context.camera.position = {0, 0, cameraZ};
        const auto geometry = DrawTransformGizmoGeometry(context.DrawList(), context.request);
        REQUIRE(geometry.HasValue());
        REQUIRE(geometry.Value().movePlaneCorners[2]);
        ImVec2 pointer{};
        for (const auto corner : *geometry.Value().movePlaneCorners[2]) {
            pointer.x += corner.x * 0.25F;
            pointer.y += corner.y * 0.25F;
        }
        auto request = context.request;
        request.pointer = pointer;
        const auto selected = DrawTransformGizmoGeometry(context.DrawList(), request);
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().hoveredAxis == 6);
        const auto point = ProjectTransformGizmoPlanePoint(
            {.camera = context.camera, .center = {}, .normal = {0, 0, 1}, .pointer = pointer, .width = 400, .height = 400});
        REQUIRE(point.HasValue());
        REQUIRE(point.Value());
        REQUIRE(std::fabs(point.Value()->z) < 0.0001F);
    }
}

TEST_CASE("Move arrows use the actual off-center perspective projection", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.request.activeWorldPosition = Math::Vec3{1.1F, 0.6F, 0};
    context.request.hovered = false;
    const auto flags = context.DrawList().Flags;
    const int firstVertex = context.DrawList().VtxBuffer.Size;
    const auto geometry = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(geometry.HasValue());
    const auto &frame = geometry.Value();
    const float extent = MoveOracleWorldExtent(context.request, frame.worldPosition);
    for (int axis = 0; axis < 3; ++axis) {
        REQUIRE(frame.projectedAxisVisible[axis]);
        const auto rawTip = ProjectMoveOracle(context.request, frame.worldPosition + frame.worldAxes[axis] * extent);
        REQUIRE(frame.moveAxisSegments[axis]);
        const auto &segment = *frame.moveAxisSegments[axis];
        auto tip = rawTip;
        if (!segment.tipVisible) {
            // This off-center X tip crosses the right viewport edge; independently intersect its projected line.
            REQUIRE(rawTip.x > context.request.origin.x + context.request.width);
            const float edge = context.request.origin.x + context.request.width;
            const float fraction = (edge - frame.center->x) / (rawTip.x - frame.center->x);
            tip = {edge, frame.center->y + (rawTip.y - frame.center->y) * fraction};
        }
        const float length = frame.pixelsPerWorldUnit[axis] * extent * static_cast<float>(segment.lastParameter - segment.firstParameter);
        RequireNearbyGizmoPoint({segment.start.x + frame.screenDirections[axis].x * length,
                                 segment.start.y + frame.screenDirections[axis].y * length},
                                tip, 0.01F);
        float nearest = std::numeric_limits<float>::max();
        for (int vertex = firstVertex; vertex < context.DrawList().VtxBuffer.Size; ++vertex) {
            const auto position = context.DrawList().VtxBuffer[vertex].pos;
            nearest = std::min(nearest, std::hypot(position.x - tip.x, position.y - tip.y));
        }
        // Genuine tips retain exact vertices; a clipped flat shaft has only the normal AA cap fringe around its endpoint.
        REQUIRE(nearest < (segment.tipVisible ? 0.01F : 1.0F));
        REQUIRE(context.DrawList().Flags == flags);
        auto hover = context.request;
        hover.hovered = true;
        hover.pointer = ProjectMoveOracle(hover, frame.worldPosition + frame.worldAxes[axis] * (extent * 0.6F));
        const auto selected = DrawTransformGizmoGeometry(context.DrawList(), hover);
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().hoveredAxis == axis);
    }
}

TEST_CASE("Move arrows and planes stay continuous across the former angle cutoff", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.camera.position = {1.3F, 0, 4};
    const auto before = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    context.camera.position.x = 1.35F;
    const auto after = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(before.HasValue());
    REQUIRE(after.HasValue());
    REQUIRE(before.Value().projectedAxisVisible[2]);
    REQUIRE(after.Value().projectedAxisVisible[2]);
    REQUIRE(before.Value().movePlaneCorners[2]);
    REQUIRE(after.Value().movePlaneCorners[2]);
    for (std::size_t corner = 0; corner < 4; ++corner)
        RequireNearbyGizmoPoint((*before.Value().movePlaneCorners[2])[corner], (*after.Value().movePlaneCorners[2])[corner], 3.0F);
}

TEST_CASE("Move projection rejects truly edge-on planes and fully behind pivots", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.request.activeWorldPosition = Math::Vec3{0, 0, 3.85F};
    const auto near = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(near.HasValue());
    REQUIRE(near.Value().center);
    REQUIRE_FALSE(near.Value().projectedAxisVisible[2]);
    REQUIRE_FALSE(near.Value().movePlaneCorners[0]);
    REQUIRE_FALSE(near.Value().movePlaneCorners[1]);
    context.request.activeWorldPosition = Math::Vec3{0, 0, 5};
    const int firstVertex = context.DrawList().VtxBuffer.Size;
    const auto behind = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(behind.HasValue());
    REQUIRE_FALSE(behind.Value().center);
    REQUIRE_FALSE(behind.Value().hoveredAxis);
    REQUIRE(context.DrawList().VtxBuffer.Size == firstVertex);
}

TEST_CASE("Move arrows extend beyond rotation and scale handles", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.camera.position = {4.0F, 3.0F, 5.0F};

    const std::array<float, 3> toolExtents{
        MeasureTransformGizmoToolExtent(context.DrawList(), context.request, EditorTransformTool::Move),
        MeasureTransformGizmoToolExtent(context.DrawList(), context.request, EditorTransformTool::Rotate),
        MeasureTransformGizmoToolExtent(context.DrawList(), context.request, EditorTransformTool::Scale),
    };
    for (const float extent : toolExtents)
        REQUIRE(extent >= 78.0F);
    REQUIRE(toolExtents[0] > toolExtents[1] + 25.0F);
    REQUIRE(toolExtents[0] > toolExtents[2] + 25.0F);
    REQUIRE(std::fabs(toolExtents[1] - toolExtents[2]) < 18.0F);
}

TEST_CASE("Transform gizmo scale handles remain stable under camera changes", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    context.camera.position = {4.0F, 3.0F, 5.0F};

    TransformGizmoGeometryRequest scaleRequest = context.request;
    scaleRequest.tool = EditorTransformTool::Scale;
    scaleRequest.pointer = {200.0F, 200.0F};
    const Result<TransformGizmoFrameGeometry> uniformScale = DrawTransformGizmoGeometry(drawList, scaleRequest);
    REQUIRE(uniformScale.HasValue());
    REQUIRE(uniformScale.Value().hoveredAxis == 3);

    const Result<Math::Vec3> scaleViewDirection = Math::TryNormalize(context.camera.target - context.camera.position);
    REQUIRE(scaleViewDirection.HasValue());
    const float xAlignment = Math::Dot(uniformScale.Value().worldAxes[0], scaleViewDirection.Value());
    const float xScreenLength = 96.0F * std::sqrt(1.0F - xAlignment * xAlignment);
    scaleRequest.pointer = {uniformScale.Value().center->x + uniformScale.Value().screenDirections[0].x * (xScreenLength - 9.0F),
                            uniformScale.Value().center->y + uniformScale.Value().screenDirections[0].y * (xScreenLength - 9.0F)};
    const Result<TransformGizmoFrameGeometry> axisScale = DrawTransformGizmoGeometry(drawList, scaleRequest);
    REQUIRE(axisScale.HasValue());
    REQUIRE(axisScale.Value().hoveredAxis == 0);

    const Result<Math::Vec3> strafeDirection = Math::TryNormalize(Math::Cross(scaleViewDirection.Value(), context.camera.up));
    REQUIRE(strafeDirection.HasValue());
    const Math::Vec3 strafeDelta = strafeDirection.Value() * 0.3F;
    for (const EditorTransformTool tool : {EditorTransformTool::Scale}) {
        TransformGizmoGeometryRequest stableRequest = context.request;
        stableRequest.tool = tool;
        stableRequest.hovered = false;
        const Result<TransformGizmoFrameGeometry> beforeStrafe = DrawTransformGizmoGeometry(drawList, stableRequest);
        REQUIRE(beforeStrafe.HasValue());
        context.camera.position = context.camera.position + strafeDelta;
        context.camera.target = context.camera.target + strafeDelta;
        const Result<TransformGizmoFrameGeometry> afterStrafe = DrawTransformGizmoGeometry(drawList, stableRequest);
        REQUIRE(afterStrafe.HasValue());
        for (int axis = 0; axis < 3; ++axis) {
            REQUIRE(std::fabs(beforeStrafe.Value().screenDirections[axis].x - afterStrafe.Value().screenDirections[axis].x) < 0.0001F);
            REQUIRE(std::fabs(beforeStrafe.Value().screenDirections[axis].y - afterStrafe.Value().screenDirections[axis].y) < 0.0001F);
            REQUIRE(std::fabs(beforeStrafe.Value().pixelsPerWorldUnit[axis] - afterStrafe.Value().pixelsPerWorldUnit[axis]) < 0.0001F);
        }
        context.camera.position = context.camera.position - strafeDelta;
        context.camera.target = context.camera.target - strafeDelta;
    }
}

TEST_CASE("Scale handles project cube corners from one 3D frame and share their picking geometry", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    for (const auto space : {EditorTransformSpace::World, EditorTransformSpace::Local}) {
        context.worldTransform =
            Math::Transform{
                .rotation = Math::Quaternion::FromEulerRadians({0.2F, 0.3F, -0.15F}),
                .scale = {2.0F, 0.75F, 1.5F},
            }
                .ToMatrix();
        for (const auto projection : {Runtime::CameraProjection::Perspective, Runtime::CameraProjection::Orthographic}) {
            context.camera.projection = projection;
            for (const Math::Vec3 eye : {Math::Vec3{4, 3, 5}, Math::Vec3{-5, 2, 4}, Math::Vec3{4, -3, -5}, Math::Vec3{0.4F, 1, 5}}) {
                context.camera.position = eye;
                const auto forward = Math::TryNormalize(context.camera.target - eye).Value();
                const auto right = Math::TryNormalize(Math::Cross(forward, context.camera.up)).Value();
                const auto up = Math::TryNormalize(Math::Cross(right, forward)).Value();
                auto request = context.request;
                request.tool = EditorTransformTool::Scale;
                request.space = space;
                request.hovered = false;
                auto &drawList = context.DrawList();
                const int firstVertex = drawList.VtxBuffer.Size;
                const auto geometry = DrawTransformGizmoGeometry(drawList, request);
                REQUIRE(geometry.HasValue());
                REQUIRE(geometry.Value().center);
                const auto center = *geometry.Value().center;
                for (int axis = 0; axis < 3; ++axis) {
                    if (!geometry.Value().projectedAxisVisible[axis])
                        continue;
                    const auto cubeCenter = geometry.Value().worldAxes[axis] * 87.0F;
                    const auto project = [&](const Math::Vec3 point) {
                        return ImVec2{center.x + Math::Dot(point, right), center.y - Math::Dot(point, up)};
                    };
                    ImRect bounds{center, center};
                    for (int corner = 0; corner < 8; ++corner) {
                        auto point = cubeCenter;
                        for (int component = 0; component < 3; ++component)
                            point = point + geometry.Value().worldAxes[component] * ((corner & (1 << component)) ? 9.0F : -9.0F);
                        bounds.Add(project(point));
                    }
                    bounds.Expand(5.0F);
                    for (int vertex = firstVertex; vertex < drawList.VtxBuffer.Size; ++vertex) {
                        const auto &drawn = drawList.VtxBuffer[vertex];
                        const auto color = ImGui::ColorConvertU32ToFloat4(drawn.col);
                        const std::array components{color.x, color.y, color.z};
                        if (color.w > 0 && components[axis] > 0.25F && components[axis] > components[(axis + 1) % 3] * 1.2F &&
                            components[axis] > components[(axis + 2) % 3] * 1.2F) {
                            CAPTURE(axis, eye.x, eye.y, eye.z, drawn.pos.x, drawn.pos.y, bounds.Min.x, bounds.Min.y, bounds.Max.x,
                                    bounds.Max.y);
                            CHECK(bounds.Contains(drawn.pos));
                        }
                    }
                    // Each visible corner must belong to the actual projected equal-edge cube,
                    // including side faces that a screen-aligned square cannot represent.
                    for (int corner = 0; corner < 8; ++corner) {
                        auto point = cubeCenter;
                        bool visible{};
                        for (int component = 0; component < 3; ++component) {
                            const float sign = (corner & (1 << component)) ? 1.0F : -1.0F;
                            point = point + geometry.Value().worldAxes[component] * (9.0F * sign);
                            visible |= Math::Dot(geometry.Value().worldAxes[component] * sign, forward * -1.0F) > 0.0001F;
                        }
                        if (!visible)
                            continue;
                        const auto expected = project(point);
                        float distance = std::numeric_limits<float>::max();
                        for (int vertex = firstVertex; vertex < drawList.VtxBuffer.Size; ++vertex) {
                            const auto &drawn = drawList.VtxBuffer[vertex];
                            const auto color = ImGui::ColorConvertU32ToFloat4(drawn.col);
                            const std::array components{color.x, color.y, color.z};
                            if (color.w == 0 || components[axis] <= 0.25F || components[axis] <= components[(axis + 1) % 3] * 1.2F ||
                                components[axis] <= components[(axis + 2) % 3] * 1.2F)
                                continue;
                            distance = std::min(distance, std::hypot(drawn.pos.x - expected.x, drawn.pos.y - expected.y));
                        }
                        REQUIRE(distance < 1.2F);
                    }
                    request.pointer = project(cubeCenter);
                    request.hovered = true;
                    const auto hovered = DrawTransformGizmoGeometry(drawList, request);
                    REQUIRE(hovered.HasValue());
                    CHECK(hovered.Value().hoveredAxis == axis);
                    request.hovered = false;
                }
            }
        }
    }
}

TEST_CASE("Overlapping scale cubes pick the camera-nearest axis", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    for (const float sign : {-1.0F, 1.0F}) {
        context.camera.position = {4 * sign, 0, -4 * sign};
        auto request = context.request;
        request.tool = EditorTransformTool::Scale;
        request.hovered = false;
        const auto frame = DrawTransformGizmoGeometry(context.DrawList(), request);
        REQUIRE(frame.HasValue());
        const int nearest = sign > 0 ? 0 : 2;
        const auto direction = frame.Value().screenDirections[nearest];
        request.pointer = {frame.Value().center->x + direction.x * 87.0F / std::sqrt(2.0F), frame.Value().center->y};
        request.hovered = true;
        const auto hovered = DrawTransformGizmoGeometry(context.DrawList(), request);
        REQUIRE(hovered.HasValue());
        CHECK(hovered.Value().hoveredAxis == nearest);
    }
}

TEST_CASE("Transform gizmo scale handles remain bounded near end-on views", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();

    context.camera.target = {};
    context.camera.position = {1.4F, 0.0F, 4.0F};
    const auto blueFromRight = MeasureBlueTransformGizmoScaleHandle(drawList, context.camera, context.worldTransform);
    context.camera.position = {-1.4F, 0.0F, 4.0F};
    const auto blueFromLeft = MeasureBlueTransformGizmoScaleHandle(drawList, context.camera, context.worldTransform);
    REQUIRE(blueFromRight.first > 25.0F);
    REQUIRE(blueFromRight.first < 48.0F);
    REQUIRE(blueFromLeft.first > 25.0F);
    REQUIRE(blueFromLeft.first < 48.0F);
    REQUIRE(blueFromRight.second.x * blueFromLeft.second.x < 0.0F);

    context.camera.position = {0.0F, 0.0F, 99.0F};
    context.camera.farPlane = 200.0F;
    const TransformGizmoGeometryRequest distantRequest{
        .camera = context.camera,
        .worldTransform = context.worldTransform,
        .tool = EditorTransformTool::Move,
        .space = EditorTransformSpace::World,
        .width = 400.0F,
        .height = 400.0F,
        .pointer = {265.0F, 200.0F},
        .hovered = true,
    };
    const Result<TransformGizmoFrameGeometry> distant = DrawTransformGizmoGeometry(drawList, distantRequest);
    REQUIRE(distant.HasValue());
    REQUIRE(distant.Value().screenDirections[0].x > 0.0F);
    REQUIRE(distant.Value().screenDirections[1].y < 0.0F);
}

TEST_CASE("Transform gizmo rotation rings preserve radius and crossings", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    context.camera.position = {0.0F, 0.0F, 4.0F};

    TransformGizmoGeometryRequest rotationRequest = context.request;
    rotationRequest.tool = EditorTransformTool::Rotate;
    rotationRequest.pointer = {267.9F, 132.1F};
    rotationRequest.hovered = false;
    const int firstRotationVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> rotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(rotation.HasValue());
    REQUIRE_FALSE(rotation.Value().hoveredAxis.has_value());
    const auto colors = TransformGizmoRotationColors();
    RequireMatchedTransformGizmoRotationRadii(
        MeasureTransformGizmoRotationRadii(drawList, firstRotationVertex, colors, *rotation.Value().center));

    rotationRequest.hovered = true;
    const Result<TransformGizmoFrameGeometry> hoveredRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(hoveredRotation.HasValue());
    REQUIRE(hoveredRotation.Value().hoveredAxis == 2);

    context.camera.position = {4.0F, 3.0F, 5.0F};
    rotationRequest.hovered = false;
    const int firstAngledVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> angledRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(angledRotation.HasValue());
    RequireMatchedTransformGizmoRotationRadii(
        MeasureTransformGizmoRotationRadii(drawList, firstAngledVertex, colors, *angledRotation.Value().center));

    const Result<Math::Vec3> forward = Math::TryNormalize(context.camera.target - context.camera.position);
    REQUIRE(forward.HasValue());
    const Result<Math::Vec3> right = Math::TryNormalize(Math::Cross(forward.Value(), context.camera.up));
    REQUIRE(right.HasValue());
    const Result<Math::Vec3> up = Math::TryNormalize(Math::Cross(right.Value(), forward.Value()));
    REQUIRE(up.HasValue());
    RequireTransformGizmoRotationRingCrossings(drawList, firstAngledVertex, *angledRotation.Value().center, colors, right.Value(),
                                               up.Value());

    const ImVec2 rearCrossing{angledRotation.Value().center->x - 96.0F * right.Value().x,
                              angledRotation.Value().center->y + 96.0F * up.Value().x};
    rotationRequest.pointer = rearCrossing;
    rotationRequest.hovered = true;
    const Result<TransformGizmoFrameGeometry> rearHover = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(rearHover.HasValue());
    REQUIRE_FALSE(rearHover.Value().hoveredAxis.has_value());
}

TEST_CASE("Transform gizmo rotation drag isolates its axis and draws an angle sector", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    context.camera.position = {4.0F, 3.0F, 5.0F};
    TransformGizmoGeometryRequest request = context.request;
    request.tool = EditorTransformTool::Rotate;
    request.activeAxis = 0;
    const int firstVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, request);
    REQUIRE(geometry.HasValue());
    REQUIRE(geometry.Value().center.has_value());
    const auto colors = TransformGizmoRotationColors();
    std::array<bool, 3> axisDrawn{};
    for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex)
        for (int axis = 0; axis < 3; ++axis)
            axisDrawn[axis] = axisDrawn[axis] || drawList.VtxBuffer[vertexIndex].col == colors[axis];
    REQUIRE(drawList.VtxBuffer.Size > firstVertex);
    REQUIRE_FALSE(axisDrawn[1]);
    REQUIRE_FALSE(axisDrawn[2]);

    const int firstSweepVertex = drawList.VtxBuffer.Size;
    REQUIRE(DrawTransformGizmoRotationSweep(drawList, context.camera, *geometry.Value().center, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F})
                .HasValue());
    const ImU32 fill = ImGui::GetColorU32(ImVec4{0.82F, 0.84F, 0.88F, 0.22F});
    bool sectorDrawn = false;
    for (int vertexIndex = firstSweepVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex)
        sectorDrawn = sectorDrawn || drawList.VtxBuffer[vertexIndex].col == fill;
    REQUIRE(sectorDrawn);
}

TEST_CASE("Transform gizmo rotation pins follow strafed rings", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    context.camera.position = {4.0F, 3.0F, 5.0F};

    TransformGizmoGeometryRequest rotationRequest = context.request;
    rotationRequest.tool = EditorTransformTool::Rotate;
    rotationRequest.pointer = {267.9F, 132.1F};
    rotationRequest.hovered = false;
    const int firstAngledVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> angledRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(angledRotation.HasValue());
    REQUIRE(angledRotation.Value().center.has_value());

    const auto colors = TransformGizmoRotationColors();
    const auto beforeStrafe = CollectTransformGizmoRotationRingOffsets(drawList, firstAngledVertex, *angledRotation.Value().center, colors);
    const Result<Math::Vec3> forward = Math::TryNormalize(context.camera.target - context.camera.position);
    REQUIRE(forward.HasValue());
    const Result<Math::Vec3> right = Math::TryNormalize(Math::Cross(forward.Value(), context.camera.up));
    REQUIRE(right.HasValue());
    const Result<Math::Vec3> up = Math::TryNormalize(Math::Cross(right.Value(), forward.Value()));
    REQUIRE(up.HasValue());

    context.camera.position.x += 0.3F;
    context.camera.target.x += 0.3F;
    const int firstStrafedVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> strafedRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(strafedRotation.HasValue());
    REQUIRE(strafedRotation.Value().center.has_value());
    const auto afterStrafe =
        CollectTransformGizmoRotationRingOffsets(drawList, firstStrafedVertex, *strafedRotation.Value().center, colors);
    for (int axis = 0; axis < 3; ++axis) {
        REQUIRE_FALSE(beforeStrafe[axis].empty());
        REQUIRE(beforeStrafe[axis].size() == afterStrafe[axis].size());
        float maximumDeviation = 0.0F;
        for (std::size_t vertexIndex = 0; vertexIndex < beforeStrafe[axis].size(); ++vertexIndex) {
            const ImVec2 before = beforeStrafe[axis][vertexIndex];
            const ImVec2 after = afterStrafe[axis][vertexIndex];
            maximumDeviation = std::max(maximumDeviation, std::hypot(before.x - after.x, before.y - after.y));
        }
        REQUIRE(maximumDeviation < 0.05F);
    }

    const std::array<Math::Vec3, 3> worldBasis{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    RequireTransformGizmoRotationPin(drawList, context.camera, *strafedRotation.Value().center, worldBasis[1], 0, colors, right.Value(),
                                     up.Value());
    RequireTransformGizmoRotationPin(drawList, context.camera, *strafedRotation.Value().center, worldBasis[2], 1, colors, right.Value(),
                                     up.Value());
    REQUIRE(DrawTransformGizmoRotationPin(drawList, context.camera, *strafedRotation.Value().center, worldBasis[0], 3).HasError());
}

TEST_CASE("Move end-on transition keeps actual projection anchor and a truthful world-axis depth gesture",
          "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.request.hovered = false;
    std::optional<ImVec2> previous;
    bool depth = false, projected = false;
    for (int step = 0; step <= 16; ++step) {
        context.camera.position = {static_cast<float>(step) * 0.00625F, 0, 4};
        const float extent = MoveOracleWorldExtent(context.request, {});
        const auto expected = ProjectMoveOracle(context.request, {0, 0, extent});
        const auto pivot = ProjectMoveOracle(context.request, {});
        const float actualLength = std::hypot(expected.x - pivot.x, expected.y - pivot.y);
        const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().moveAxisSegments[2]);
        const auto anchor = frame.Value().moveEndOnAnchors[2].value_or(frame.Value().moveAxisSegments[2]->end);
        RequireNearbyGizmoPoint(anchor, expected, 0.01F);
        if (previous)
            REQUIRE(std::hypot(anchor.x - previous->x, anchor.y - previous->y) < 1.0F);
        previous = anchor;
        const bool isDepth = frame.Value().moveAxisGestures[2] == TransformGizmoAxisGesture::ViewDepth;
        REQUIRE(isDepth == (actualLength < 2.0F));
        depth = depth || isDepth;
        projected = projected || !isDepth;
        auto request = context.request;
        request.hovered = true;
        request.pointer = anchor;
        const auto picked = DrawTransformGizmoGeometry(context.DrawList(), request);
        REQUIRE(picked.HasValue());
        REQUIRE(picked.Value().hoveredAxis == 2);
    }
    REQUIRE(depth);
    REQUIRE(projected);
    context.camera.position = {0, 0, 4};
    const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(frame.HasValue());
    const auto drag = BeginTransformGizmoMath({.tool = EditorTransformTool::Move,
                                               .space = EditorTransformSpace::World,
                                               .axis = 2,
                                               .worldAxis = frame.Value().worldAxes[2],
                                               .pixelsPerWorldUnit = frame.Value().pixelsPerWorldUnit[2]});
    REQUIRE(drag.HasValue());
    const ImVec2 pointerDelta{0, -12};
    const auto gesture = frame.Value().screenDirections[2];
    const auto moved =
        EvaluateTransformGizmoMath(drag.Value(), {.projectedPixels = pointerDelta.x * gesture.x + pointerDelta.y * gesture.y});
    REQUIRE(moved.HasValue());
    REQUIRE(moved.Value().worldPosition.x == 0);
    REQUIRE(moved.Value().worldPosition.y == 0);
    REQUIRE(moved.Value().worldPosition.z > 0);
}

TEST_CASE("Move shafts preserve actual near and far intersections in both backend depth conventions",
          "[unit][editor][viewport][gizmo][clip]") {
    ImGuiGizmoGeometryTestContext context;
    for (const auto projection : {Runtime::CameraProjection::Perspective, Runtime::CameraProjection::Orthographic}) {
        context.camera.projection = projection;
        for (const auto range : {Math::ClipDepthRange::NegativeOneToOne, Math::ClipDepthRange::ZeroToOne}) {
            context.request.depthRange = range;
            for (const bool near : {true, false}) {
                context.camera.position = near ? Math::Vec3{3, 3, 4} : Math::Vec3{-3, -3, -4};
                const auto forward = Math::TryNormalize(context.camera.target - context.camera.position);
                REQUIRE(forward.HasValue());
                const float depth = projection == Runtime::CameraProjection::Perspective
                                        ? (near ? context.camera.nearPlane * 1.04F : context.camera.farPlane * 0.98F)
                                        : (near ? context.camera.nearPlane + 0.2F : context.camera.farPlane - 0.2F);
                const auto pivot = context.camera.position + forward.Value() * depth;
                context.request.activeWorldPosition = pivot;
                context.request.hovered = false;
                const float extent = MoveOracleWorldExtent(context.request, pivot);
                const auto end = pivot + Math::Vec3{0, 0, extent};
                const auto intersection =
                    IntersectMoveDepthOracle(context.camera, pivot, end, near ? context.camera.nearPlane : context.camera.farPlane);
                const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
                REQUIRE(frame.HasValue());
                REQUIRE(frame.Value().moveAxisSegments[2]);
                const auto &segment = *frame.Value().moveAxisSegments[2];
                REQUIRE_FALSE(segment.tipVisible);
                REQUIRE(segment.lastParameter < 1.0);
                REQUIRE(frame.Value().projectedAxisVisible[2]);
                RequireNearbyGizmoPoint(segment.end, ProjectMoveBoundaryOracle(context.request, intersection), 0.05F);
                auto hover = context.request;
                hover.hovered = true;
                hover.pointer = {segment.start.x + (segment.end.x - segment.start.x) * 0.75F,
                                 segment.start.y + (segment.end.y - segment.start.y) * 0.75F};
                const auto selected = DrawTransformGizmoGeometry(context.DrawList(), hover);
                REQUIRE(selected.HasValue());
                REQUIRE(selected.Value().hoveredAxis == 2);
            }
        }
    }
}

TEST_CASE("Move clipped planes retain visible draw and pick polygons with independently checked depth edges",
          "[unit][editor][viewport][gizmo][clip]") {
    ImGuiGizmoGeometryTestContext context;
    for (const auto projection : {Runtime::CameraProjection::Perspective, Runtime::CameraProjection::Orthographic}) {
        context.camera.projection = projection;
        for (const auto range : {Math::ClipDepthRange::NegativeOneToOne, Math::ClipDepthRange::ZeroToOne}) {
            context.request.depthRange = range;
            for (const bool near : {true, false}) {
                context.camera.position = near ? Math::Vec3{3, 3, 4} : Math::Vec3{-3, -3, -4};
                const auto forward = Math::TryNormalize(context.camera.target - context.camera.position);
                REQUIRE(forward.HasValue());
                const float depth = projection == Runtime::CameraProjection::Perspective
                                        ? (near ? context.camera.nearPlane * 1.04F : context.camera.farPlane * 0.98F)
                                        : (near ? context.camera.nearPlane + 0.2F : context.camera.farPlane - 0.2F);
                const auto pivot = context.camera.position + forward.Value() * depth;
                context.request.activeWorldPosition = pivot;
                context.request.hovered = false;
                const auto extent = MoveOracleWorldExtent(context.request, pivot);
                const auto first = pivot + Math::Vec3{-0.14F * extent, 0, 0.14F * extent};
                const auto outside = pivot + Math::Vec3{-0.14F * extent, 0, 0.32F * extent};
                const auto crossing =
                    IntersectMoveDepthOracle(context.camera, first, outside, near ? context.camera.nearPlane : context.camera.farPlane);
                const int firstVertex = context.DrawList().VtxBuffer.Size;
                const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
                REQUIRE(frame.HasValue());
                REQUIRE(frame.Value().movePlaneCorners[1]);
                const auto &polygon = *frame.Value().movePlaneCorners[1];
                REQUIRE(polygon.count == 5);
                const auto expected = ProjectMoveBoundaryOracle(context.request, crossing);
                float nearest = std::numeric_limits<float>::max();
                for (const auto point : polygon)
                    nearest = std::min(nearest, std::hypot(point.x - expected.x, point.y - expected.y));
                REQUIRE(nearest < 0.05F);
                RequireMovePolygonDrawn(context.DrawList(), firstVertex, polygon);
                auto hover = context.request;
                hover.hovered = true;
                hover.pointer = MovePolygonCenter(polygon);
                const auto picked = DrawTransformGizmoGeometry(context.DrawList(), hover);
                REQUIRE(picked.HasValue());
                REQUIRE(picked.Value().hoveredAxis == 5);
            }
        }
    }
}

TEST_CASE("Move fully clipped and degenerate geometry creates no phantom pointer regions", "[unit][editor][viewport][gizmo][clip]") {
    ImGuiGizmoGeometryTestContext context;
    for (const auto range : {Math::ClipDepthRange::NegativeOneToOne, Math::ClipDepthRange::ZeroToOne}) {
        context.request.depthRange = range;
        for (const auto point : {Math::Vec3{0, 0, 5}, Math::Vec3{0, 0, -200}}) {
            context.request.activeWorldPosition = point;
            context.request.pointer = {200, 200};
            const int before = context.DrawList().VtxBuffer.Size;
            const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
            REQUIRE(frame.HasValue());
            REQUIRE_FALSE(frame.Value().center);
            REQUIRE_FALSE(frame.Value().hoveredAxis);
            REQUIRE(context.DrawList().VtxBuffer.Size == before);
        }
    }
    context.request.activeWorldPosition.reset();
    const auto frame = DrawTransformGizmoGeometry(context.DrawList(), context.request);
    REQUIRE(frame.HasValue());
    REQUIRE_FALSE(frame.Value().movePlaneCorners[0]);
    REQUIRE_FALSE(frame.Value().movePlaneCorners[1]);
    auto hover = context.request;
    hover.pointer = {-1, 200};
    const auto outside = DrawTransformGizmoGeometry(context.DrawList(), hover);
    REQUIRE(outside.HasValue());
    REQUIRE_FALSE(outside.Value().hoveredAxis);
}

TEST_CASE("Move clipping reuses fixed scratch and prepared ImGui draw storage without frame allocations",
          "[unit][editor][viewport][gizmo][clip]") {
    ImGuiGizmoGeometryTestContext context;
    context.camera.position = {3, 3, 4};
    const auto forward = Math::TryNormalize(context.camera.target - context.camera.position);
    REQUIRE(forward.HasValue());
    context.request.activeWorldPosition = context.camera.position + forward.Value() * (context.camera.nearPlane * 1.04F);
    context.request.hovered = false;
    REQUIRE(DrawTransformGizmoGeometry(context.DrawList(), context.request).HasValue());
    auto &list = context.DrawList();
    list.VtxBuffer.reserve(list.VtxBuffer.Size + 4096);
    list.IdxBuffer.reserve(list.IdxBuffer.Size + 8192);
    list.CmdBuffer.reserve(list.CmdBuffer.Size + 8);
    std::size_t allocations = 0;
    bool success = false;
    {
        GizmoAllocationProbe probe;
        const auto drawn = DrawTransformGizmoGeometry(list, context.request);
        success = drawn.HasValue();
        allocations = probe.allocations;
    }
    REQUIRE(success);
    REQUIRE(allocations == 0);
}
