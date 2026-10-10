#include "Horo/Runtime/Render/LightCulling.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Canonical world-space unit cube, including its boundary. */
    LightCluster Cube() {
        return {.planes = {LightClusterPlane{{1, 0, 0}, 1}, LightClusterPlane{{-1, 0, 0}, 1}, LightClusterPlane{{0, 1, 0}, 1},
                           LightClusterPlane{{0, -1, 0}, 1}, LightClusterPlane{{0, 0, 1}, 1}, LightClusterPlane{{0, 0, -1}, 1}}};
    }

    /** @brief Produce a valid light with an explicit stable identity. */
    IdentifiedRenderLight Light(const std::uint64_t identity, const RenderLightKind kind = RenderLightKind::Directional) {
        return {{identity}, {.kind = kind, .range = 1.0F}};
    }
}  // namespace

TEST_CASE("CPU cluster membership is conservative for all runtime light families", "[runtime][renderer][light-culling]") {
    const auto cluster = Cube();
    REQUIRE(cluster.IsValid());
    auto directional = Light(1);
    directional.light.position = {1000, 0, 0};
    CHECK(LightIntersectsCluster(directional.light, cluster));
    auto punctual = Light(2, RenderLightKind::Point);
    CHECK(LightIntersectsCluster(punctual.light, cluster));
    punctual.light.position = {2, 0, 0};
    CHECK(LightIntersectsCluster(punctual.light, cluster));
    punctual.light.position.x = 2.01F;
    CHECK_FALSE(LightIntersectsCluster(punctual.light, cluster));
    punctual.light.kind = RenderLightKind::Spot;
    punctual.light.position = {};
    CHECK(LightIntersectsCluster(punctual.light, cluster));
    punctual.light.range = 0;
    CHECK_FALSE(LightIntersectsCluster(punctual.light, cluster));
    directional.light.intensity = 0;
    CHECK_FALSE(LightIntersectsCluster(directional.light, cluster));
}

TEST_CASE("Baseline selection feeds existing render submission without widening forward capacity", "[runtime][renderer][light-culling]") {
    std::array<IdentifiedRenderLight, 17> lights;
    for (std::size_t index = 0; index < lights.size(); ++index)
        lights[index] = Light(index + 1);
    std::array<RenderLight, MaximumForwardLights> output;
    LightCullingBudget budget{.maximumLights = 17, .maximumClusters = 1, .referencesPerCluster = 16};
    REQUIRE(PrepareForwardLights(lights, Cube(), budget, output).HasError());
    budget.requireCompleteCoverage = false;
    const auto selected = PrepareForwardLights(lights, Cube(), budget, output);
    REQUIRE(selected.HasValue());
    CHECK(selected.Value().count == MaximumForwardLights);
    CHECK(selected.Value().omitted == 1);
    const RenderSceneView scene{.lights = std::span{output}.first(selected.Value().count)};
    CHECK(scene.IsValid());
    budget.referencesPerCluster = 17;
    CHECK(PrepareForwardLights(lights, Cube(), budget, output).ErrorValue().code.Value() == LightCullingErrors::Unsupported.code.Value());
}

TEST_CASE("CPU lists use exact canonical table indices and finite per-cluster ranges", "[runtime][renderer][light-culling]") {
    auto outside = Light(20, RenderLightKind::Point);
    outside.light.position = {30, 0, 0};
    const std::array lights{Light(10), outside, Light(30, RenderLightKind::Spot)};
    const std::array clusters{Cube(), Cube()};
    std::array<LightClusterMembership, 2> membership;
    std::array<std::uint32_t, 6> indices{99, 99, 99, 99, 99, 99};
    const LightCullingBudget budget{.maximumLights = 3, .maximumClusters = 2, .referencesPerCluster = 3};
    REQUIRE(CullLightsCpu(lights, clusters, budget, membership, indices).HasValue());
    CHECK(membership[0].offset == 0);
    CHECK(membership[1].offset == 3);
    CHECK(membership[0].count == 2);
    CHECK(membership[1].count == 2);
    CHECK(membership[0].omitted == 0);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 2);
    CHECK(indices[2] == 99);
    CHECK(indices[3] == 0);
    CHECK(indices[4] == 2);
}

TEST_CASE("Light overflow requires declared degradation or fails complete coverage", "[runtime][renderer][light-culling]") {
    const std::array lights{Light(10), Light(20), Light(30)};
    const std::array clusters{Cube()};
    std::array<LightClusterMembership, 1> membership;
    std::array<std::uint32_t, 2> indices;
    LightCullingBudget budget{.maximumLights = 3, .maximumClusters = 1, .referencesPerCluster = 2};
    const auto required = CullLightsCpu(lights, clusters, budget, membership, indices);
    REQUIRE(required.HasError());
    CHECK(required.ErrorValue().code.Value() == LightCullingErrors::Coverage.code.Value());
    budget.requireCompleteCoverage = false;
    REQUIRE(CullLightsCpu(lights, clusters, budget, membership, indices).HasValue());
    CHECK(membership[0].count == 2);
    CHECK(membership[0].omitted == 1);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
}

TEST_CASE("Malformed lights and planes fail before writing cluster lists", "[runtime][renderer][light-culling]") {
    std::array lights{Light(10), Light(20)};
    std::array clusters{Cube()};
    std::array<LightClusterMembership, 1> membership{{{.count = 17}}};
    std::array<std::uint32_t, 2> indices{99, 99};
    const LightCullingBudget budget{.maximumLights = 2, .maximumClusters = 1, .referencesPerCluster = 2};
    SECTION("Duplicate identity") {
        lights[1].identity = lights[0].identity;
    }
    SECTION("Unordered identity") {
        lights[1].identity.value = 9;
    }
    SECTION("Zero identity") {
        lights[0].identity.value = 0;
    }
    SECTION("Unknown kind") {
        lights[0].light.kind = static_cast<RenderLightKind>(255);
    }
    SECTION("Nonfinite intensity") {
        lights[0].light.intensity = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("Degenerate plane") {
        clusters[0].planes[0].normal = {};
    }
    SECTION("Nonfinite plane") {
        clusters[0].planes[0].offset = std::numeric_limits<float>::infinity();
    }
    const auto result = CullLightsCpu(lights, clusters, budget, membership, indices);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == LightCullingErrors::InvalidInput.code.Value());
    CHECK(membership[0].count == 17);
    CHECK(indices[0] == 99);
}

TEST_CASE("Light culling honors admission bounds cancellation and empty frames", "[runtime][renderer][light-culling]") {
    const std::array lights{Light(10), Light(20)};
    const std::array clusters{Cube()};
    std::array<LightClusterMembership, 1> membership;
    std::array<std::uint32_t, 2> indices;
    const LightCullingBudget budget{.maximumLights = 2, .maximumClusters = 1, .referencesPerCluster = 2};
    CHECK(CullLightsCpu(lights, clusters, budget, membership, std::span{indices}.first(1)).HasError());
    CHECK(CullLightsCpu(lights, clusters, budget, {}, indices).HasError());
    CHECK_FALSE((LightCullingBudget{.maximumLights = 0}).IsValid());
    CHECK_FALSE((LightCullingBudget{.maximumLights = 4096, .maximumClusters = 4096, .referencesPerCluster = 4096}).IsValid());
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto cancelled = CullLightsCpu(lights, clusters, budget, membership, indices, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == LightCullingErrors::Cancelled.code.Value());
    REQUIRE(CullLightsCpu({}, clusters, budget, membership, indices).HasValue());
    CHECK(membership[0].count == 0);
    REQUIRE(CullLightsCpu({}, {}, budget, {}, {}).HasValue());
}
