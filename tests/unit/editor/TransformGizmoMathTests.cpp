#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoGeometry.h"
#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoMath.h"

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
