#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/renderer/EditorViewportScene.h"
#include "editor/renderer/grid/EditorViewportGridGeometry.h"
#include "editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace {
    [[nodiscard]] bool NearlyEqual(const float lhs, const float rhs) noexcept {
        return std::fabs(lhs - rhs) < 0.0001F;
    }

    TEST_CASE("Transform Uses Translation Rotation Scale Order", "[unit][editor]") {
        using namespace Horo::Math;
        const Transform transform{
            .translation = {2.0F, 3.0F, 4.0F},
            .rotation = Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 1.57079632679489661923F),
            .scale = {2.0F, 2.0F, 2.0F},
        };
        const Vec3 result = TransformPoint(transform.ToMatrix(), {1.0F, 0.0F, 0.0F});
        REQUIRE((NearlyEqual(result.x, 2.0F)));
        REQUIRE((NearlyEqual(result.y, 3.0F)));
        REQUIRE((NearlyEqual(result.z, 2.0F)));
    }

    TEST_CASE("Directional Shadow View Fits Submitted World Bounds", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        Runtime::PrimitiveMeshCache cache;
        auto acquired = cache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box));
        REQUIRE(acquired.HasValue());
        Runtime::PrimitiveMeshLease lease = std::move(acquired).Value();
        const Render::MeshData &mesh = lease.Data();
        const Render::RenderMeshSourceHandle meshHandle{Render::MeshResourceId{1}, 1};
        const std::array resources{EditorViewportMeshResourceView{meshHandle, mesh.vertices, mesh.indices, mesh.localBounds}};
        const std::array instances{
            EditorViewportInstance{meshHandle,
                                   Math::TranslationMatrix({-4.0F, 0.0F, 2.0F}),
                                   mesh.localBounds,
                                   Render::CoreDefaultMaterial,
                                   {}},
            EditorViewportInstance{meshHandle,
                                   Math::TranslationMatrix({6.0F, 3.0F, -2.0F}),
                                   mesh.localBounds,
                                   Render::CoreDefaultMaterial,
                                   {}},
        };
        const std::array lights{
            Render::RenderLight{
                .kind = Render::RenderLightKind::Point,
                .position = {0.0F, 3.0F, 0.0F},
            },
            Render::RenderLight{
                .kind = Render::RenderLightKind::Directional,
                .direction = Math::Normalize(Math::Vec3{-1.0F, -2.0F, -1.0F}),
            },
        };
        const Render::RenderSceneView scene{
            .camera = ToRenderCamera(EditorViewportCamera{}),
            .meshResources = resources,
            .instances = instances,
            .lights = lights,
        };

        const auto openGlShadow = BuildEditorViewportDirectionalShadowView(scene, Math::ClipDepthRange::NegativeOneToOne);
        const auto metalShadow = BuildEditorViewportDirectionalShadowView(scene, Math::ClipDepthRange::ZeroToOne);
        REQUIRE(openGlShadow.HasValue());
        REQUIRE(openGlShadow.Value().has_value());
        REQUIRE(openGlShadow.Value()->lightIndex == 1);
        REQUIRE(openGlShadow.Value()->IsValid(scene));
        REQUIRE(metalShadow.HasValue());
        REQUIRE(metalShadow.Value().has_value());
        REQUIRE(metalShadow.Value()->lightIndex == 1);
        REQUIRE(metalShadow.Value()->IsValid(scene));

        const Math::Vec4 worldCenter{-4.0F, 0.0F, 2.0F, 1.0F};
        const Math::Vec4 glClip = Math::TransformHomogeneous(openGlShadow.Value()->viewProjection, worldCenter);
        const Math::Vec4 metalClip = Math::TransformHomogeneous(metalShadow.Value()->viewProjection, worldCenter);
        REQUIRE(std::abs(glClip.x / glClip.w) <= 1.0F);
        REQUIRE(std::abs(glClip.y / glClip.w) <= 1.0F);
        REQUIRE(glClip.z / glClip.w >= -1.0F);
        REQUIRE(glClip.z / glClip.w <= 1.0F);
        REQUIRE(metalClip.z / metalClip.w >= 0.0F);
        REQUIRE(metalClip.z / metalClip.w <= 1.0F);
    }

    TEST_CASE("Look At Uses Right Handed Negative Z View Space", "[unit][editor]") {
        using namespace Horo::Math;
        const Mat4 view = LookAt({0.0F, 0.0F, 4.0F}, {}, {0.0F, 1.0F, 0.0F});
        const Vec3 originInView = TransformPoint(view, {});
        REQUIRE((NearlyEqual(originInView.x, 0.0F)));
        REQUIRE((NearlyEqual(originInView.y, 0.0F)));
        REQUIRE((NearlyEqual(originInView.z, -4.0F)));
    }

    TEST_CASE("Perspective Makes Clip Depth Explicit", "[unit][editor]") {
        using namespace Horo::Math;
        constexpr float nearPlane = 0.1F;
        constexpr float farPlane = 100.0F;
        constexpr float fov = 0.9599310885968813F;
        const Mat4 openGl = Perspective(fov, 1.0F, nearPlane, farPlane, ClipDepthRange::NegativeOneToOne);
        const Mat4 zeroToOne = Perspective(fov, 1.0F, nearPlane, farPlane, ClipDepthRange::ZeroToOne);
        REQUIRE((NearlyEqual(TransformPoint(openGl, {0.0F, 0.0F, -nearPlane}).z, -1.0F)));
        REQUIRE((NearlyEqual(TransformPoint(openGl, {0.0F, 0.0F, -farPlane}).z, 1.0F)));
        REQUIRE((NearlyEqual(TransformPoint(zeroToOne, {0.0F, 0.0F, -nearPlane}).z, 0.0F)));
        REQUIRE((NearlyEqual(TransformPoint(zeroToOne, {0.0F, 0.0F, -farPlane}).z, 1.0F)));
    }

    TEST_CASE("Viewport Scene Validates Typed Inputs And Shared Geometry", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        Runtime::PrimitiveMeshCache cache;
        auto acquired = cache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box));
        REQUIRE((acquired.HasValue()));
        Runtime::PrimitiveMeshLease lease = std::move(acquired).Value();
        const Render::MeshData &mesh = lease.Data();
        const Render::RenderMeshSourceHandle meshHandle{lease.Id(), 1};
        const std::array resources{EditorViewportMeshResourceView{meshHandle, mesh.vertices, mesh.indices, mesh.localBounds}};
        const std::array instances{
            EditorViewportInstance{meshHandle, Math::Mat4::Identity(), mesh.localBounds, Render::CoreDefaultMaterial, {}}};
        const EditorViewportSceneView valid{.camera = {}, .meshResources = resources, .instances = instances};
        REQUIRE((valid.IsValid()));
        REQUIRE((mesh.vertices.size() == 24));
        REQUIRE((mesh.indices.size() == 36));

        EditorViewportCamera invalidCamera;
        invalidCamera.nearPlane = std::numeric_limits<float>::quiet_NaN();
        const EditorViewportSceneView invalid{.camera = invalidCamera, .meshResources = resources, .instances = instances};
        REQUIRE((!invalid.IsValid()));

        const std::array lights{Render::RenderLight{
            .kind = Render::RenderLightKind::Spot,
            .position = {2.0F, 3.0F, 4.0F},
            .direction = Math::Normalize(Math::Vec3{-1.0F, -1.0F, -1.0F}),
            .color = {1.0F, 0.8F, 0.6F},
            .intensity = 2.0F,
            .range = 16.0F,
            .innerConeCosine = 0.9F,
            .outerConeCosine = 0.7F,
        }};
        const EditorViewportSceneView lit{.camera = {}, .meshResources = resources, .instances = instances, .lights = lights};
        REQUIRE((lit.IsValid()));
        Render::RenderLight invalidLight = lights.front();
        invalidLight.direction = {};
        const std::array invalidLights{invalidLight};
        const EditorViewportSceneView invalidLit{
            .camera = {},
            .meshResources = resources,
            .instances = instances,
            .lights = invalidLights,
        };
        REQUIRE((!invalidLit.IsValid()));
    }

    TEST_CASE("Viewport Projection And Rays Share The Camera Contract", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using enum Math::ClipDepthRange;
        EditorViewportCamera perspective;
        const Result<Math::Ray> perspectiveOpenGl = BuildEditorViewportRay(perspective, 0.5F, 0.5F, 1.0F, NegativeOneToOne);
        const Result<Math::Ray> perspectiveZeroToOne = BuildEditorViewportRay(perspective, 0.5F, 0.5F, 1.0F, ZeroToOne);
        REQUIRE((perspectiveOpenGl.HasValue() && perspectiveZeroToOne.HasValue()));
        REQUIRE((Math::NearlyEqual(perspectiveOpenGl.Value().origin, perspective.position)));
        REQUIRE((Math::NearlyEqual(perspectiveOpenGl.Value().direction, Math::Normalize(perspective.target - perspective.position))));
        REQUIRE((Math::NearlyEqual(perspectiveOpenGl.Value().origin, perspectiveZeroToOne.Value().origin)));
        REQUIRE((Math::NearlyEqual(perspectiveOpenGl.Value().direction, perspectiveZeroToOne.Value().direction)));

        EditorViewportCamera orthographic = perspective;
        orthographic.projection = Runtime::CameraProjection::Orthographic;
        orthographic.orthographicHeight = 4.0F;
        const Result<Math::Ray> centerOpenGl = BuildEditorViewportRay(orthographic, 0.5F, 0.5F, 1.0F, NegativeOneToOne);
        const Result<Math::Ray> rightOpenGl = BuildEditorViewportRay(orthographic, 0.75F, 0.5F, 1.0F, NegativeOneToOne);
        const Result<Math::Ray> centerZeroToOne = BuildEditorViewportRay(orthographic, 0.5F, 0.5F, 1.0F, ZeroToOne);
        const Result<Math::Ray> rightZeroToOne = BuildEditorViewportRay(orthographic, 0.75F, 0.5F, 1.0F, ZeroToOne);
        REQUIRE((centerOpenGl.HasValue() && rightOpenGl.HasValue()));
        REQUIRE((centerZeroToOne.HasValue() && rightZeroToOne.HasValue()));
        REQUIRE((Math::NearlyEqual(centerOpenGl.Value().direction, rightOpenGl.Value().direction)));
        REQUIRE((rightOpenGl.Value().origin.x > centerOpenGl.Value().origin.x));
        REQUIRE((Math::NearlyEqual(centerOpenGl.Value().origin, centerZeroToOne.Value().origin, 1e-4F)));
        REQUIRE((Math::NearlyEqual(rightOpenGl.Value().origin, rightZeroToOne.Value().origin, 1e-4F)));
        REQUIRE((Math::NearlyEqual(centerOpenGl.Value().direction, centerZeroToOne.Value().direction)));

        const Result<Math::Mat4> openGl = BuildEditorViewportViewProjection(orthographic, 1.0F, NegativeOneToOne);
        const Result<Math::Mat4> metal = BuildEditorViewportViewProjection(orthographic, 1.0F, ZeroToOne);
        REQUIRE((openGl.HasValue() && metal.HasValue()));
        REQUIRE((!Math::NearlyEqual(openGl.Value().values[10], metal.Value().values[10])));

        const Result<std::optional<EditorViewportPointProjection>> projectedOpenGl =
            ProjectEditorViewportPoint(perspective, {}, 1.0F, NegativeOneToOne);
        const Result<std::optional<EditorViewportPointProjection>> projectedZeroToOne =
            ProjectEditorViewportPoint(perspective, {}, 1.0F, ZeroToOne);
        REQUIRE((projectedOpenGl.HasValue() && projectedOpenGl.Value().has_value()));
        REQUIRE((projectedZeroToOne.HasValue() && projectedZeroToOne.Value().has_value()));
        REQUIRE((Math::NearlyEqual(projectedOpenGl.Value()->viewportPosition, {0.5F, 0.5F})));
        REQUIRE((Math::NearlyEqual(projectedOpenGl.Value()->viewportPosition, projectedZeroToOne.Value()->viewportPosition)));

        const Result<std::optional<EditorViewportPointProjection>> behind =
            ProjectEditorViewportPoint(perspective, {0.0F, 0.0F, 10.0F}, 1.0F, ZeroToOne);
        REQUIRE((behind.HasValue() && !behind.Value().has_value()));

        EditorViewportCamera degenerate = perspective;
        degenerate.target = degenerate.position;
        REQUIRE((ProjectEditorViewportPoint(degenerate, {}, 1.0F, NegativeOneToOne).HasError()));
        REQUIRE((BuildEditorViewportRay(perspective, 0.5F, 0.5F, 0.0F, NegativeOneToOne).HasError()));
    }

    TEST_CASE("Viewport Grid Keeps A Stable Screen Density Across Camera Distances", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        ViewportGridGeometry nearGrid;
        const EditorViewportCamera nearCamera{};
        REQUIRE((BuildViewportGridGeometry(
            ViewportGridGeometryRequest{
                .camera = ToRenderCamera(nearCamera),
                .aspect = 16.0F / 9.0F,
                .viewportHeightPixels = 480.0F,
                .targetMinorSpacingPixels = 48.0F,
            },
            nearGrid)));
        REQUIRE((nearGrid.IsValid()));
        REQUIRE((NearlyEqual(nearGrid.minorSpacing, 0.5F)));
        REQUIRE((nearGrid.RegularLines().topology == ViewportGridPrimitiveTopology::Triangles));
        REQUIRE((NearlyEqual(nearGrid.RegularLines().color.x, 0.158F)));
        REQUIRE((nearGrid.regularVertexCount % 6 == 0));
        REQUIRE((nearGrid.axisVertexCount == 4));
        float minimumX = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = std::numeric_limits<float>::lowest();
        const auto includePosition = [&](const Math::Vec3 position) {
            minimumX = std::min(minimumX, position.x);
            maximumX = std::max(maximumX, position.x);
            minimumZ = std::min(minimumZ, position.z);
            maximumZ = std::max(maximumZ, position.z);
        };
        for (const Math::Vec3 position : nearGrid.RegularLines().positions)
            includePosition(position);
        for (const Math::Vec3 position : nearGrid.Axes().positions)
            includePosition(position);
        const float farHalfHeight = nearCamera.farPlane * std::tan(nearCamera.verticalFovRadians * 0.5F);
        const float farHalfWidth = farHalfHeight * (16.0F / 9.0F);
        const float farCornerRadius =
            std::sqrt(nearCamera.farPlane * nearCamera.farPlane + farHalfWidth * farHalfWidth + farHalfHeight * farHalfHeight);
        REQUIRE((minimumX <= nearCamera.position.x - farCornerRadius));
        REQUIRE((maximumX >= nearCamera.position.x + farCornerRadius));
        REQUIRE((minimumZ <= nearCamera.position.z - farCornerRadius));
        REQUIRE((maximumZ >= nearCamera.position.z + farCornerRadius));

        EditorViewportCamera farCamera = nearCamera;
        farCamera.position = {0.0F, 0.0F, 40.0F};
        ViewportGridGeometry farGrid;
        REQUIRE((BuildViewportGridGeometry(
            ViewportGridGeometryRequest{
                .camera = ToRenderCamera(farCamera),
                .aspect = 16.0F / 9.0F,
                .viewportHeightPixels = 480.0F,
                .targetMinorSpacingPixels = 48.0F,
            },
            farGrid)));
        REQUIRE((farGrid.minorSpacing > nearGrid.minorSpacing));
        REQUIRE((NearlyEqual(farGrid.minorSpacing, 5.0F)));
        REQUIRE((farGrid.regularVertexCount <= ViewportGridGeometry::MaxRegularVertices));

        ViewportGridGeometry ultraWideGrid;
        REQUIRE((BuildViewportGridGeometry(
            ViewportGridGeometryRequest{
                .camera = ToRenderCamera(nearCamera),
                .aspect = 100.0F,
                .viewportHeightPixels = 480.0F,
                .targetMinorSpacingPixels = 48.0F,
            },
            ultraWideGrid)));
        REQUIRE((ultraWideGrid.regularVertexCount <= ViewportGridGeometry::MaxRegularVertices));
    }

    TEST_CASE("Viewport Grid Uses Orthographic Height And Rejects Invalid Requests", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        EditorViewportCamera camera;
        camera.projection = Runtime::CameraProjection::Orthographic;
        camera.orthographicHeight = 10.0F;
        camera.target = {12.25F, 0.0F, -7.75F};
        ViewportGridGeometry grid;
        REQUIRE((BuildViewportGridGeometry(
            ViewportGridGeometryRequest{
                .camera = ToRenderCamera(camera),
                .aspect = 1.0F,
                .viewportHeightPixels = 500.0F,
                .targetMinorSpacingPixels = 50.0F,
            },
            grid)));
        REQUIRE((NearlyEqual(grid.minorSpacing, 1.0F)));
        REQUIRE((grid.IsValid()));

        ViewportGridGeometry invalidGrid;
        REQUIRE_FALSE((BuildViewportGridGeometry(
            ViewportGridGeometryRequest{
                .camera = ToRenderCamera(camera),
                .aspect = 0.0F,
                .viewportHeightPixels = 500.0F,
                .targetMinorSpacingPixels = 50.0F,
            },
            invalidGrid)));
        REQUIRE((invalidGrid.regularVertexCount == 0));
    }

    TEST_CASE("Selected Light Visualizers Produce Bounded Geometry For Every Light Kind", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const Render::RenderCameraView camera = ToRenderCamera(EditorViewportCamera{});
        for (const Render::RenderLightKind kind :
             {Render::RenderLightKind::Directional, Render::RenderLightKind::Point, Render::RenderLightKind::Spot}) {
            Render::RenderLight light{
                .kind = kind,
                .position = {1.0F, 2.0F, 3.0F},
                .direction = {0.0F, -1.0F, 0.0F},
                .color = {1.0F, 0.5F, 0.2F},
                .intensity = 2.0F,
                .range = 8.0F,
                .innerConeCosine = std::cos(0.25F),
                .outerConeCosine = std::cos(0.65F),
            };
            LightVisualizerGeometry geometry;
            REQUIRE((BuildLightVisualizerGeometry({.camera = camera, .light = light}, geometry)));
            REQUIRE((geometry.IsValid()));
            REQUIRE((!geometry.Lines().empty()));
            REQUIRE((geometry.vertexCount <= LightVisualizerGeometry::MaximumVertexCount));
        }

        Render::RenderLight invalid;
        invalid.range = -1.0F;
        LightVisualizerGeometry geometry;
        REQUIRE_FALSE((BuildLightVisualizerGeometry({.camera = camera, .light = invalid}, geometry)));
        REQUIRE((geometry.vertexCount == 0));

        for (const Render::RenderLightKind kind : {Render::RenderLightKind::Point, Render::RenderLightKind::Spot}) {
            Render::RenderLight boundaryLight{
                .kind = kind,
                .position = {1.0F, 2.0F, 3.0F},
                .direction = {0.0F, -1.0F, 0.0F},
                .range = 0.0F,
                .innerConeCosine = -1.0F,
                .outerConeCosine = -1.0F,
            };
            REQUIRE((BuildLightVisualizerGeometry({.camera = camera, .light = boundaryLight}, geometry)));
            REQUIRE((geometry.IsValid()));
        }
    }
}  // namespace
