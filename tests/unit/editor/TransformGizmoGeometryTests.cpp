#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoGeometry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
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
                                                                                const Math::Mat4 &worldTransform,
                                                                                const ImU32 blueScaleColor) {
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
            if (vertex.col == blueScaleColor)
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
    REQUIRE_FALSE(hub.Value().hoveredAxis.has_value());

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
    endOnRequest.pointer = {222.0F, 178.0F};
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

TEST_CASE("Move plane handles stay behind their matching arrows across camera angles", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    for (const Math::Vec3 cameraPosition : std::array{Math::Vec3{4.0F, 3.0F, 4.0F}, Math::Vec3{-4.0F, 3.0F, 4.0F}}) {
        context.camera.position = cameraPosition;
        const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, context.request);
        REQUIRE(geometry.HasValue());
        REQUIRE(geometry.Value().center.has_value());
        for (int axis = 0; axis < 3; ++axis) {
            REQUIRE(geometry.Value().projectedAxisVisible[axis]);
            REQUIRE(geometry.Value().movePlaneCorners[axis].has_value());
            const ImVec2 direction = geometry.Value().screenDirections[axis];
            ImVec2 handleCenter{};
            for (const ImVec2 corner : *geometry.Value().movePlaneCorners[axis]) {
                const ImVec2 offset{corner.x - geometry.Value().center->x, corner.y - geometry.Value().center->y};
                REQUIRE(offset.x * direction.x + offset.y * direction.y <= -7.9F);
                handleCenter.x += corner.x * 0.25F;
                handleCenter.y += corner.y * 0.25F;
            }
            TransformGizmoGeometryRequest hoverRequest = context.request;
            hoverRequest.pointer = handleCenter;
            const Result<TransformGizmoFrameGeometry> hovered = DrawTransformGizmoGeometry(drawList, hoverRequest);
            REQUIRE(hovered.HasValue());
            REQUIRE(hovered.Value().hoveredAxis == axis + 4);
        }
    }
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
    for (const EditorTransformTool tool : {EditorTransformTool::Move, EditorTransformTool::Scale}) {
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

TEST_CASE("Scale handle faces stay square to their projected axes", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    context.camera.position = {4.0F, 3.0F, 5.0F};
    TransformGizmoGeometryRequest request = context.request;
    request.tool = EditorTransformTool::Scale;
    request.hovered = false;

    ImDrawList &drawList = context.DrawList();
    const int firstVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, request);
    REQUIRE(geometry.HasValue());
    REQUIRE(geometry.Value().center.has_value());

    const Result<Math::Vec3> viewDirection = Math::TryNormalize(context.camera.target - context.camera.position);
    REQUIRE(viewDirection.HasValue());
    const ImU32 redFace = ImGui::GetColorU32(ImVec4{0.93F, 0.29F, 0.26F, 1.0F});
    const Math::Vec3 worldAxis = geometry.Value().worldAxes[0];
    const float alignment = Math::Dot(worldAxis, viewDirection.Value());
    const float axisLength = 96.0F * std::sqrt(1.0F - alignment * alignment);
    const ImVec2 direction = geometry.Value().screenDirections[0];
    const ImVec2 handle{geometry.Value().center->x + direction.x * (axisLength - 9.0F),
                        geometry.Value().center->y + direction.y * (axisLength - 9.0F)};
    for (const float along : {-9.0F, 9.0F}) {
        for (const float across : {-9.0F, 9.0F}) {
            const ImVec2 corner{handle.x + direction.x * along - direction.y * across,
                                handle.y + direction.y * along + direction.x * across};
            float nearest = std::numeric_limits<float>::max();
            for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
                const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
                if (vertex.col == redFace)
                    nearest = std::min(nearest, std::hypot(vertex.pos.x - corner.x, vertex.pos.y - corner.y));
            }
            REQUIRE(nearest < 1.0F);
        }
    }
}

TEST_CASE("Transform gizmo scale handles remain bounded near end-on views", "[unit][editor][viewport][gizmo]") {
    ImGuiGizmoGeometryTestContext context;
    ImDrawList &drawList = context.DrawList();
    const ImU32 blueScaleColor = ImGui::GetColorU32(ImVec4{0.25F, 0.51F, 0.96F, 1.0F});

    context.camera.target = {};
    context.camera.position = {1.4F, 0.0F, 4.0F};
    const auto blueFromRight = MeasureBlueTransformGizmoScaleHandle(drawList, context.camera, context.worldTransform, blueScaleColor);
    context.camera.position = {-1.4F, 0.0F, 4.0F};
    const auto blueFromLeft = MeasureBlueTransformGizmoScaleHandle(drawList, context.camera, context.worldTransform, blueScaleColor);
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
