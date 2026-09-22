#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoGeometry.h"
#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoMath.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    [[nodiscard]] BeginTransformGizmoMathRequest MakeRequest() {
        return BeginTransformGizmoMathRequest{
            .tool = EditorTransformTool::Move,
            .space = EditorTransformSpace::Local,
            .axis = 0,
            .initialLocalTransform = {},
            .initialWorldTransform = Math::Mat4::Identity(),
            .parentWorldTransform = Math::Mat4::Identity(),
            .worldAxis = {1.0F, 0.0F, 0.0F},
            .pixelsPerWorldUnit = 50.0F,
        };
    }
}  // namespace

TEST_CASE("Transform gizmo rejects singular parents before a drag begins", "[unit][editor][viewport][gizmo]") {
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.parentWorldTransform = Math::Transform{.scale = {0.0F, 1.0F, 1.0F}}.ToMatrix();

    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);

    REQUIRE(session.HasError());
}

TEST_CASE("Local gizmo scale preserves negative authored scale", "[unit][editor][viewport][gizmo]") {
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.tool = EditorTransformTool::Scale;
    request.initialLocalTransform.scale = {-2.0F, 1.0F, 1.0F};
    request.initialWorldTransform = request.initialLocalTransform.ToMatrix();
    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);
    REQUIRE(session.HasValue());

    const Result<TransformGizmoMathOutcome> outcome =
        EvaluateTransformGizmoMath(session.Value(), TransformGizmoMathUpdate{.projectedPixels = 120.0F});

    REQUIRE(outcome.HasValue());
    REQUIRE(outcome.Value().localTransform.scale.x < -2.0F);
    REQUIRE(outcome.Value().localTransform.scale.y == 1.0F);
    REQUIRE(outcome.Value().localTransform.scale.z == 1.0F);
}

TEST_CASE("Move gizmo maps a world displacement through the parent inverse", "[unit][editor][viewport][gizmo]") {
    const Math::Transform parent{
        .translation = {4.0F, -2.0F, 1.0F},
        .rotation = Math::Quaternion::FromEulerRadians({0.0F, 0.35F, 0.0F}),
        .scale = {2.0F, 0.75F, 1.5F},
    };
    const Math::Transform local{
        .translation = {1.0F, 2.0F, -3.0F},
    };
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.initialLocalTransform = local;
    request.parentWorldTransform = parent.ToMatrix();
    request.initialWorldTransform = Math::Multiply(parent.ToMatrix(), local.ToMatrix());
    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);
    REQUIRE(session.HasValue());

    const Result<TransformGizmoMathOutcome> outcome =
        EvaluateTransformGizmoMath(session.Value(), TransformGizmoMathUpdate{.projectedPixels = 100.0F});

    REQUIRE(outcome.HasValue());
    const Math::Mat4 resolvedWorld = Math::Multiply(parent.ToMatrix(), outcome.Value().localTransform.ToMatrix());
    const Math::Vec3 resolvedPosition = Math::TransformPoint(resolvedWorld, {});
    REQUIRE((Math::NearlyEqual(resolvedPosition, session.Value().initialWorldPosition + Math::Vec3{2.0F, 0.0F, 0.0F}, 1e-5F)));
    REQUIRE((Math::NearlyEqual(outcome.Value().worldPosition, resolvedPosition, 1e-5F)));
}

TEST_CASE("World gizmo scale remains representable under a rotated non-uniform parent", "[unit][editor][viewport][gizmo]") {
    const Math::Transform parent{
        .rotation = Math::Quaternion::FromEulerRadians({0.0F, 0.35F, 0.0F}),
        .scale = {2.0F, 0.75F, 1.5F},
    };
    const Math::Transform local{
        .translation = {1.0F, 2.0F, -3.0F},
        .rotation = Math::Quaternion::FromEulerRadians({0.1F, -0.2F, 0.3F}),
        .scale = {-1.0F, 1.25F, 0.5F},
    };
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.tool = EditorTransformTool::Scale;
    request.space = EditorTransformSpace::World;
    request.initialLocalTransform = local;
    request.parentWorldTransform = parent.ToMatrix();
    request.initialWorldTransform = Math::Multiply(parent.ToMatrix(), local.ToMatrix());
    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);
    REQUIRE(session.HasValue());

    const Result<TransformGizmoMathOutcome> outcome =
        EvaluateTransformGizmoMath(session.Value(), TransformGizmoMathUpdate{.projectedPixels = 60.0F});

    REQUIRE(outcome.HasValue());
    REQUIRE(outcome.Value().localTransform.TryToMatrix().HasValue());
    REQUIRE(outcome.Value().localTransform.scale.x < 0.0F);
    REQUIRE(outcome.Value().localTransform.scale.y > 0.0F);
    REQUIRE(outcome.Value().localTransform.scale.z > 0.0F);
}

TEST_CASE("World gizmo rotation evaluates through a non-uniform parent", "[unit][editor][viewport][gizmo]") {
    const Math::Transform parent{
        .rotation = Math::Quaternion::FromEulerRadians({0.0F, 0.4F, 0.0F}),
        .scale = {2.0F, 0.75F, 1.5F},
    };
    const Math::Transform local{
        .translation = {0.5F, 0.0F, -1.0F},
        .rotation = Math::Quaternion::FromEulerRadians({0.1F, 0.2F, 0.0F}),
    };
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.tool = EditorTransformTool::Rotate;
    request.space = EditorTransformSpace::World;
    request.axis = 1;
    request.initialLocalTransform = local;
    request.parentWorldTransform = parent.ToMatrix();
    request.initialWorldTransform = Math::Multiply(parent.ToMatrix(), local.ToMatrix());
    request.worldAxis = {0.0F, 1.0F, 0.0F};
    request.startRotationVector = Math::Vec3{1.0F, 0.0F, 0.0F};
    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);
    REQUIRE(session.HasValue());

    const Result<TransformGizmoMathOutcome> outcome =
        EvaluateTransformGizmoMath(session.Value(), TransformGizmoMathUpdate{
                                                        .currentRotationVector = Math::Vec3{0.0F, 0.0F, -1.0F},
                                                    });

    REQUIRE(outcome.HasValue());
    REQUIRE(outcome.Value().localTransform.TryToMatrix().HasValue());
    REQUIRE(outcome.Value().localTransform.rotation != local.rotation);
}

TEST_CASE("Transform gizmo reports invalid axes and missing rotation vectors", "[unit][editor][viewport][gizmo]") {
    BeginTransformGizmoMathRequest invalidAxis = MakeRequest();
    invalidAxis.axis = 3;
    const Result<TransformGizmoMathSession> invalidSession = BeginTransformGizmoMath(invalidAxis);
    REQUIRE(invalidSession.HasError());
    REQUIRE(invalidSession.ErrorValue().code.Value() == "transform_gizmo.invalid_request");

    BeginTransformGizmoMathRequest missingVector = MakeRequest();
    missingVector.tool = EditorTransformTool::Rotate;
    const Result<TransformGizmoMathSession> missingSession = BeginTransformGizmoMath(missingVector);
    REQUIRE(missingSession.HasError());
    REQUIRE(missingSession.ErrorValue().code.Value() == "transform_gizmo.rotation_vector_required");
}

TEST_CASE("Local gizmo axes reject singular transforms while world axes stay explicit", "[unit][editor][viewport][gizmo]") {
    const Math::Mat4 singular = Math::Transform{.scale = {0.0F, 1.0F, 1.0F}}.ToMatrix();

    const Result<std::array<Math::Vec3, 3>> local = ResolveTransformGizmoWorldAxes(singular, EditorTransformSpace::Local);
    const Result<std::array<Math::Vec3, 3>> world = ResolveTransformGizmoWorldAxes(singular, EditorTransformSpace::World);

    REQUIRE(local.HasError());
    REQUIRE(world.HasValue());
    REQUIRE((world.Value()[0] == Math::Vec3{1.0F, 0.0F, 0.0F}));
    REQUIRE((world.Value()[1] == Math::Vec3{0.0F, 1.0F, 0.0F}));
    REQUIRE((world.Value()[2] == Math::Vec3{0.0F, 0.0F, 1.0F}));
}

TEST_CASE("Transform gizmo axes reject non-finite transforms in every space", "[unit][editor][viewport][gizmo]") {
    Math::Mat4 nonFinite = Math::Mat4::Identity();
    nonFinite.values[12] = std::numeric_limits<float>::infinity();

    const Result<std::array<Math::Vec3, 3>> local = ResolveTransformGizmoWorldAxes(nonFinite, EditorTransformSpace::Local);
    const Result<std::array<Math::Vec3, 3>> world = ResolveTransformGizmoWorldAxes(nonFinite, EditorTransformSpace::World);

    REQUIRE(local.HasError());
    REQUIRE(world.HasError());
    REQUIRE(local.ErrorValue().code.Value() == "transform_gizmo.invalid_request");
    REQUIRE(world.ErrorValue().code.Value() == "transform_gizmo.invalid_request");
}

TEST_CASE("Transform gizmo rotation projection distinguishes misses from invalid inputs", "[unit][editor][viewport][gizmo]") {
    EditorViewportCamera camera;
    const Result<std::optional<Math::Vec3>> miss = ProjectTransformGizmoRotationVector({.camera = camera,
                                                                                        .center = {},
                                                                                        .normal = {1.0F, 0.0F, 0.0F},
                                                                                        .pointer = {50.0F, 50.0F},
                                                                                        .origin = {},
                                                                                        .width = 100.0F,
                                                                                        .height = 100.0F});
    REQUIRE((miss.HasValue()));
    REQUIRE((!miss.Value().has_value()));

    const Result<std::optional<Math::Vec3>> invalid = ProjectTransformGizmoRotationVector(
        {.camera = camera, .center = {}, .normal = {}, .pointer = {50.0F, 50.0F}, .origin = {}, .width = 100.0F, .height = 100.0F});
    REQUIRE((invalid.HasError()));
    REQUIRE((ProjectTransformGizmoRotationVector({.camera = camera, .width = 0.0F, .height = 100.0F}).HasError()));
}

TEST_CASE("Linear gizmo identifies axes without a stable arrow direction", "[unit][editor][viewport][gizmo]") {
    EditorViewportCamera camera;

    const Result<bool> xVisible = HasTransformGizmoLinearAxisScreenDirection(camera, {1.0F, 0.0F, 0.0F});
    const Result<bool> yVisible = HasTransformGizmoLinearAxisScreenDirection(camera, {0.0F, 1.0F, 0.0F});
    const Result<bool> zVisible = HasTransformGizmoLinearAxisScreenDirection(camera, {0.0F, 0.0F, 1.0F});

    REQUIRE(xVisible.HasValue());
    REQUIRE(yVisible.HasValue());
    REQUIRE(zVisible.HasValue());
    REQUIRE(xVisible.Value());
    REQUIRE(yVisible.Value());
    REQUIRE_FALSE(zVisible.Value());
}

TEST_CASE("Linear gizmo axis visibility is invariant under camera strafe", "[unit][editor][viewport][gizmo]") {
    EditorViewportCamera camera;
    const Result<bool> before = HasTransformGizmoLinearAxisScreenDirection(camera, {0.0F, 0.0F, 1.0F});
    camera.position.x += 3.0F;
    camera.target.x += 3.0F;
    const Result<bool> after = HasTransformGizmoLinearAxisScreenDirection(camera, {0.0F, 0.0F, 1.0F});

    REQUIRE(before.HasValue());
    REQUIRE(after.HasValue());
    REQUIRE(before.Value() == after.Value());
    REQUIRE_FALSE(after.Value());
}

TEST_CASE("Linear gizmo axis visibility validates camera and axis inputs", "[unit][editor][viewport][gizmo]") {
    EditorViewportCamera camera;
    REQUIRE(HasTransformGizmoLinearAxisScreenDirection(camera, {}).HasError());
    camera.target = camera.position;
    REQUIRE(HasTransformGizmoLinearAxisScreenDirection(camera, {1.0F, 0.0F, 0.0F}).HasError());
}

TEST_CASE("Transform gizmo handles remain consistently sized and selectable", "[unit][editor][viewport][gizmo]") {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {400.0F, 400.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImGui::NewFrame();
    ImGui::Begin("GizmoGeometryTest");
    ImDrawList &drawList = *ImGui::GetWindowDrawList();
    EditorViewportCamera camera;
    const Math::Mat4 worldTransform = Math::Mat4::Identity();
    const TransformGizmoGeometryRequest request{
        .camera = camera,
        .worldTransform = worldTransform,
        .tool = EditorTransformTool::Move,
        .space = EditorTransformSpace::World,
        .width = 400.0F,
        .height = 400.0F,
        .pointer = {265.0F, 200.0F},
        .hovered = true,
    };

    const Result<TransformGizmoFrameGeometry> geometry = DrawTransformGizmoGeometry(drawList, request);

    REQUIRE(geometry.HasValue());
    REQUIRE(geometry.Value().hoveredAxis == 0);
    REQUIRE(geometry.Value().center.has_value());
    TransformGizmoGeometryRequest hubRequest = request;
    hubRequest.pointer = {200.0F, 200.0F};
    const Result<TransformGizmoFrameGeometry> hub = DrawTransformGizmoGeometry(drawList, hubRequest);
    REQUIRE(hub.HasValue());
    REQUIRE_FALSE(hub.Value().hoveredAxis.has_value());

    TransformGizmoGeometryRequest endOnRequest = request;
    endOnRequest.pointer = {212.75F, 212.75F};
    const Result<TransformGizmoFrameGeometry> endOnZ = DrawTransformGizmoGeometry(drawList, endOnRequest);
    REQUIRE(endOnZ.HasValue());
    REQUIRE(endOnZ.Value().hoveredAxis == 2);
    REQUIRE(endOnZ.Value().pixelsPerWorldUnit[2] > 0.0F);
    REQUIRE(endOnZ.Value().screenDirections[2].y == -1.0F);

    camera.position = {4.0F, 0.0F, 0.0F};
    const Result<TransformGizmoFrameGeometry> endOnX = DrawTransformGizmoGeometry(drawList, endOnRequest);
    REQUIRE(endOnX.HasValue());
    REQUIRE(endOnX.Value().hoveredAxis == 0);

    endOnRequest.tool = EditorTransformTool::Scale;
    const Result<TransformGizmoFrameGeometry> scaleEndOnX = DrawTransformGizmoGeometry(drawList, endOnRequest);
    REQUIRE(scaleEndOnX.HasValue());
    REQUIRE(scaleEndOnX.Value().hoveredAxis == 0);

    camera.position = {4.0F, 3.0F, 5.0F};
    const auto drawnExtent = [&](const EditorTransformTool tool) {
        TransformGizmoGeometryRequest sizedRequest = request;
        sizedRequest.tool = tool;
        sizedRequest.hovered = false;
        const int firstVertex = drawList.VtxBuffer.Size;
        const Result<TransformGizmoFrameGeometry> sized = DrawTransformGizmoGeometry(drawList, sizedRequest);
        REQUIRE(sized.HasValue());
        REQUIRE(sized.Value().center.has_value());
        float extent = 0.0F;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex)
            extent = std::max(extent, std::hypot(drawList.VtxBuffer[vertexIndex].pos.x - sized.Value().center->x,
                                                 drawList.VtxBuffer[vertexIndex].pos.y - sized.Value().center->y));
        return extent;
    };
    const std::array<float, 3> toolExtents{drawnExtent(EditorTransformTool::Move), drawnExtent(EditorTransformTool::Rotate),
                                           drawnExtent(EditorTransformTool::Scale)};
    for (const float extent : toolExtents)
        REQUIRE(extent >= 78.0F);
    REQUIRE(*std::max_element(toolExtents.begin(), toolExtents.end()) - *std::min_element(toolExtents.begin(), toolExtents.end()) < 18.0F);

    TransformGizmoGeometryRequest scaleRequest = request;
    scaleRequest.tool = EditorTransformTool::Scale;
    scaleRequest.pointer = {200.0F, 200.0F};
    const Result<TransformGizmoFrameGeometry> uniformScale = DrawTransformGizmoGeometry(drawList, scaleRequest);
    REQUIRE(uniformScale.HasValue());
    REQUIRE(uniformScale.Value().hoveredAxis == 3);
    const Result<Math::Vec3> scaleViewDirection = Math::TryNormalize(camera.target - camera.position);
    REQUIRE(scaleViewDirection.HasValue());
    const float xAlignment = Math::Dot(uniformScale.Value().worldAxes[0], scaleViewDirection.Value());
    const float xScreenLength = 96.0F * std::sqrt(1.0F - xAlignment * xAlignment);
    scaleRequest.pointer = {uniformScale.Value().center->x + uniformScale.Value().screenDirections[0].x * (xScreenLength - 9.0F),
                            uniformScale.Value().center->y + uniformScale.Value().screenDirections[0].y * (xScreenLength - 9.0F)};
    const Result<TransformGizmoFrameGeometry> axisScale = DrawTransformGizmoGeometry(drawList, scaleRequest);
    REQUIRE(axisScale.HasValue());
    REQUIRE(axisScale.Value().hoveredAxis == 0);

    // Strafing changes the projected center, not the axis shape relative to that center.
    const Result<Math::Vec3> strafeDirection = Math::TryNormalize(Math::Cross(scaleViewDirection.Value(), camera.up));
    REQUIRE(strafeDirection.HasValue());
    const Math::Vec3 strafeDelta = strafeDirection.Value() * 0.3F;
    for (const EditorTransformTool tool : {EditorTransformTool::Move, EditorTransformTool::Scale}) {
        TransformGizmoGeometryRequest stableRequest = request;
        stableRequest.tool = tool;
        stableRequest.hovered = false;
        const Result<TransformGizmoFrameGeometry> beforeStrafe = DrawTransformGizmoGeometry(drawList, stableRequest);
        REQUIRE(beforeStrafe.HasValue());
        camera.position = camera.position + strafeDelta;
        camera.target = camera.target + strafeDelta;
        const Result<TransformGizmoFrameGeometry> afterStrafe = DrawTransformGizmoGeometry(drawList, stableRequest);
        REQUIRE(afterStrafe.HasValue());
        for (int axis = 0; axis < 3; ++axis) {
            REQUIRE(std::fabs(beforeStrafe.Value().screenDirections[axis].x - afterStrafe.Value().screenDirections[axis].x) < 0.0001F);
            REQUIRE(std::fabs(beforeStrafe.Value().screenDirections[axis].y - afterStrafe.Value().screenDirections[axis].y) < 0.0001F);
            REQUIRE(std::fabs(beforeStrafe.Value().pixelsPerWorldUnit[axis] - afterStrafe.Value().pixelsPerWorldUnit[axis]) < 0.0001F);
        }
        camera.position = camera.position - strafeDelta;
        camera.target = camera.target - strafeDelta;
    }

    // Near end-on, the Z handle is foreshortened instead of swinging across the full gizmo diameter.
    const ImU32 blueScaleColor = ImGui::GetColorU32(ImVec4{0.25F, 0.51F, 0.96F, 1.0F});
    const auto blueScaleHandle = [&] {
        TransformGizmoGeometryRequest nearEndRequest = request;
        nearEndRequest.tool = EditorTransformTool::Scale;
        nearEndRequest.hovered = false;
        const int firstVertex = drawList.VtxBuffer.Size;
        const Result<TransformGizmoFrameGeometry> nearEnd = DrawTransformGizmoGeometry(drawList, nearEndRequest);
        REQUIRE(nearEnd.HasValue());
        float extent = 0.0F;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            if (vertex.col == blueScaleColor)
                extent = std::max(extent, std::hypot(vertex.pos.x - nearEnd.Value().center->x, vertex.pos.y - nearEnd.Value().center->y));
        }
        return std::pair{extent, nearEnd.Value().screenDirections[2]};
    };
    camera.target = {};
    camera.position = {1.4F, 0.0F, 4.0F};
    const auto blueFromRight = blueScaleHandle();
    camera.position = {-1.4F, 0.0F, 4.0F};
    const auto blueFromLeft = blueScaleHandle();
    REQUIRE(blueFromRight.first > 25.0F);
    REQUIRE(blueFromRight.first < 48.0F);
    REQUIRE(blueFromLeft.first > 25.0F);
    REQUIRE(blueFromLeft.first < 48.0F);
    REQUIRE(blueFromRight.second.x * blueFromLeft.second.x < 0.0F);

    camera.position = {0.0F, 0.0F, 99.0F};
    camera.farPlane = 200.0F;
    const Result<TransformGizmoFrameGeometry> distant = DrawTransformGizmoGeometry(drawList, request);
    REQUIRE(distant.HasValue());
    REQUIRE(distant.Value().screenDirections[0].x > 0.0F);
    REQUIRE(distant.Value().screenDirections[1].y < 0.0F);

    camera.position = {0.0F, 0.0F, 4.0F};
    const int firstRotationVertex = drawList.VtxBuffer.Size;
    TransformGizmoGeometryRequest rotationRequest = request;
    rotationRequest.tool = EditorTransformTool::Rotate;
    rotationRequest.pointer = {267.9F, 132.1F};
    rotationRequest.hovered = false;
    const Result<TransformGizmoFrameGeometry> rotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(rotation.HasValue());
    REQUIRE_FALSE(rotation.Value().hoveredAxis.has_value());
    const std::array<ImU32, 3> colors{
        ImGui::GetColorU32(ImVec4{0.98F, 0.42F, 0.38F, 1.0F}),
        ImGui::GetColorU32(ImVec4{0.45F, 0.88F, 0.49F, 1.0F}),
        ImGui::GetColorU32(ImVec4{0.48F, 0.64F, 1.0F, 1.0F}),
    };
    const auto measureRadii = [&](const int firstVertex) {
        std::array<float, 3> radii{};
        for (int index = firstVertex; index < drawList.VtxBuffer.Size; ++index) {
            const ImDrawVert &vertex = drawList.VtxBuffer[index];
            for (int axis = 0; axis < 3; ++axis) {
                if (vertex.col == colors[axis])
                    radii[axis] = std::max(radii[axis], std::hypot(vertex.pos.x - 200.0F, vertex.pos.y - 200.0F));
            }
        }
        return radii;
    };
    const auto requireMatchedRadii = [](const std::array<float, 3> &radii) {
        for (const float radius : radii)
            REQUIRE(radius >= 92.0F);
        for (const float radius : radii)
            REQUIRE(radius <= 102.0F);
        REQUIRE(*std::max_element(radii.begin(), radii.end()) - *std::min_element(radii.begin(), radii.end()) < 5.0F);
    };
    requireMatchedRadii(measureRadii(firstRotationVertex));
    rotationRequest.hovered = true;
    const Result<TransformGizmoFrameGeometry> hoveredRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(hoveredRotation.HasValue());
    REQUIRE(hoveredRotation.Value().hoveredAxis == 2);

    camera.position = {4.0F, 3.0F, 5.0F};
    rotationRequest.hovered = false;
    const int firstAngledVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> angledRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(angledRotation.HasValue());
    requireMatchedRadii(measureRadii(firstAngledVertex));

    const Result<Math::Vec3> forward = Math::TryNormalize(camera.target - camera.position);
    REQUIRE(forward.HasValue());
    const Result<Math::Vec3> right = Math::TryNormalize(Math::Cross(forward.Value(), camera.up));
    REQUIRE(right.HasValue());
    const Result<Math::Vec3> up = Math::TryNormalize(Math::Cross(right.Value(), forward.Value()));
    REQUIRE(up.HasValue());
    const std::array<Math::Vec3, 3> worldBasis{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    for (int sharedAxis = 0; sharedAxis < 3; ++sharedAxis) {
        const ImVec2 crossing{
            angledRotation.Value().center->x + 96.0F * Math::Dot(worldBasis[sharedAxis], right.Value()),
            angledRotation.Value().center->y - 96.0F * Math::Dot(worldBasis[sharedAxis], up.Value()),
        };
        for (int ringAxis = 0; ringAxis < 3; ++ringAxis) {
            if (ringAxis == sharedAxis)
                continue;
            float nearest = std::numeric_limits<float>::max();
            for (int vertexIndex = firstAngledVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
                const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
                if (vertex.col == colors[ringAxis])
                    nearest = std::min(nearest, std::hypot(vertex.pos.x - crossing.x, vertex.pos.y - crossing.y));
            }
            REQUIRE(nearest < 4.0F);
        }
    }

    const auto ringOffsets = [&](const int firstVertex, const ImVec2 center) {
        std::array<std::vector<ImVec2>, 3> offsets;
        for (int vertexIndex = firstVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            for (int axis = 0; axis < 3; ++axis)
                if (vertex.col == colors[axis])
                    offsets[axis].push_back({vertex.pos.x - center.x, vertex.pos.y - center.y});
        }
        return offsets;
    };
    const auto beforeStrafe = ringOffsets(firstAngledVertex, *angledRotation.Value().center);
    camera.position.x += 0.3F;
    camera.target.x += 0.3F;
    const int firstStrafedVertex = drawList.VtxBuffer.Size;
    const Result<TransformGizmoFrameGeometry> strafedRotation = DrawTransformGizmoGeometry(drawList, rotationRequest);
    REQUIRE(strafedRotation.HasValue());
    REQUIRE(strafedRotation.Value().center.has_value());
    const auto afterStrafe = ringOffsets(firstStrafedVertex, *strafedRotation.Value().center);
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

    const auto checkRotationPin = [&](const Math::Vec3 radial, const int activeAxis) {
        const int firstPinVertex = drawList.VtxBuffer.Size;
        const Result<void> pin = DrawTransformGizmoRotationPin(drawList, camera, *strafedRotation.Value().center, radial, activeAxis);
        REQUIRE(pin.HasValue());
        const ImU32 highlight = ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.9F});
        const ImVec2 expected{
            strafedRotation.Value().center->x + 96.0F * Math::Dot(radial, right.Value()),
            strafedRotation.Value().center->y - 96.0F * Math::Dot(radial, up.Value()),
        };
        bool foundAxisColor = false;
        float closestHighlight = std::numeric_limits<float>::max();
        for (int vertexIndex = firstPinVertex; vertexIndex < drawList.VtxBuffer.Size; ++vertexIndex) {
            const ImDrawVert &vertex = drawList.VtxBuffer[vertexIndex];
            foundAxisColor = foundAxisColor || vertex.col == colors[activeAxis];
            if (vertex.col == highlight)
                closestHighlight = std::min(closestHighlight, std::hypot(vertex.pos.x - expected.x, vertex.pos.y - expected.y));
        }
        REQUIRE(foundAxisColor);
        REQUIRE(closestHighlight < 2.0F);
    };
    checkRotationPin(worldBasis[1], 0);
    checkRotationPin(worldBasis[2], 1);
    REQUIRE(DrawTransformGizmoRotationPin(drawList, camera, *strafedRotation.Value().center, worldBasis[0], 3).HasError());
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
}

TEST_CASE("Transform gizmo rejects non-representable extreme updates", "[unit][editor][viewport][gizmo]") {
    BeginTransformGizmoMathRequest request = MakeRequest();
    request.pixelsPerWorldUnit = std::numeric_limits<float>::min();
    const Result<TransformGizmoMathSession> session = BeginTransformGizmoMath(request);
    REQUIRE((session.HasValue()));

    const Result<TransformGizmoMathOutcome> outcome =
        EvaluateTransformGizmoMath(session.Value(), TransformGizmoMathUpdate{.projectedPixels = std::numeric_limits<float>::max()});
    REQUIRE((outcome.HasError()));
    REQUIRE((outcome.ErrorValue().code.Value() == "math.non_finite_input"));
}
