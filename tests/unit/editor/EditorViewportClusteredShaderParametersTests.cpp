#include "EditorViewportShaderParameters.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ClusterParametersFixture {
        Render::RenderSceneView scene;
        Render::ClusteredLightGrid grid;
        std::array<Render::LightCluster, 512> cells;

        explicit ClusterParametersFixture(const std::uint32_t z = 2) {
            scene.lightSceneDomain = 7;
            scene.lightSnapshotRevision = 9;
            Render::ClusteredLightGridDescriptor descriptor;
            descriptor.identity = {{1}, 7, 9, 1, 1};
            descriptor.camera = scene.camera;
            descriptor.extent = {640, 480};
            descriptor.x = 2;
            descriptor.y = 2;
            descriptor.z = z;
            const Render::LightCullingBudget budget{.maximumLights = 1, .maximumClusters = 512, .referencesPerCluster = 1};
            const auto built = Render::BuildClusteredLightGrid(descriptor, budget, cells);
            REQUIRE(built.HasValue());
            grid = built.Value();
        }
    };
}  // namespace

TEST_CASE("Clustered parameters reject exact camera projection mismatches without mutation", "[editor][clustered-parameters]") {
    ClusterParametersFixture fixture;
    EditorViewportClusteredShaderParameters parameters;
    REQUIRE(parameters.SetScene(fixture.scene, fixture.grid, {}, 16).HasValue());
    const auto original = parameters;
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    auto &camera = fixture.scene.camera;
    if (mutation == 0)
        camera.position.x += 0.000001F;
    if (mutation == 1)
        camera.target.x += 0.000001F;
    if (mutation == 2)
        camera.up.x += 0.000001F;
    if (mutation == 3)
        camera.projection.kind = Render::RenderProjectionKind::Orthographic;
    if (mutation == 4)
        camera.projection.verticalFovRadians += 0.000001F;
    if (mutation == 5)
        camera.projection.orthographicHeight += 1;
    if (mutation == 6)
        camera.projection.nearPlane += 0.000001F;
    if (mutation == 7)
        camera.projection.farPlane += 1;
    CHECK(parameters.SetScene(fixture.scene, fixture.grid, {}, 16).HasError());
    CHECK(parameters.common.camera == original.common.camera);
    CHECK(parameters.dimensions == original.dimensions);
    CHECK(parameters.depth == original.depth);
    CHECK(parameters.viewport == original.viewport);
}

TEST_CASE("Clustered parameters reject aggregate reference overflow before publication", "[editor][clustered-parameters]") {
    ClusterParametersFixture fixture{128};
    EditorViewportClusteredShaderParameters parameters;
    REQUIRE(parameters.SetScene(fixture.scene, fixture.grid, {}, 16).HasValue());
    const auto original = parameters;
    CHECK(parameters.SetScene(fixture.scene, fixture.grid, {}, 4096).HasError());
    CHECK(parameters.viewport == original.viewport);
    CHECK(parameters.common.camera == original.common.camera);
}
