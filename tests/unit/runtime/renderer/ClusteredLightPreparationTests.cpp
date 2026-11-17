#include "ClusteredLightTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Render::ClusteredLightTests;

TEST_CASE("Prepared cluster lists retain the full canonical table and exact deterministic cell diagnostics", "[renderer][light][cluster]") {
    const auto descriptor = Descriptor();
    std::array<LightCluster, 24> clusters;
    const auto grid = Build(descriptor, clusters);
    std::array<IdentifiedRenderLight, 24> lights;
    for (unsigned z = 0; z < 4; ++z)
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 3; ++x) {
                const auto index = x + 3 * (y + 2 * z);
                lights[index] = Light(index + 1, CellPoint(descriptor, x, y, z));
            }
    const LightCullingBudget budget{.maximumLights = 24, .maximumClusters = 24, .referencesPerCluster = 2};
    std::array<LightClusterMembership, 24> membership;
    std::array<std::uint32_t, 48> references;
    const auto prepared = PrepareClusteredLightLists(grid, lights, clusters, budget, membership, references);
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().lights.size() == 24);
    CHECK(prepared.Value().diagnostics.completeCoverage);
    CHECK(prepared.Value().diagnostics.retainedReferenceCount == 24);
    for (unsigned index = 0; index < 24; ++index) {
        const auto selected = prepared.Value().ReferencesForCluster(index);
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().size() == 1);
        CHECK(selected.Value()[0] == index);
    }
    CHECK(prepared.Value().ReferencesForCluster(24).HasError());
}

TEST_CASE("Cluster preparation checks exact camera planes and reports explicit stable overflow without publishing required loss",
          "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 1;
    std::array<LightCluster, 1> clusters;
    const auto grid = Build(descriptor, clusters);
    const std::array lights{Light(1, {}, RenderLightKind::Directional), Light(2, {}, RenderLightKind::Directional)};
    LightCullingBudget budget{.maximumLights = 2, .maximumClusters = 1, .referencesPerCluster = 1};
    std::array<LightClusterMembership, 1> membership;
    std::array<std::uint32_t, 1> references;
    const auto required = PrepareClusteredLightLists(grid, lights, clusters, budget, membership, references);
    REQUIRE(required.HasError());
    CHECK(required.ErrorValue().code.Value() == LightCullingErrors::Coverage.code.Value());
    budget.requireCompleteCoverage = false;
    const auto allowed = PrepareClusteredLightLists(grid, lights, clusters, budget, membership, references);
    REQUIRE(allowed.HasValue());
    CHECK_FALSE(allowed.Value().diagnostics.completeCoverage);
    CHECK(allowed.Value().diagnostics.omittedReferenceCount == 1);
    CHECK(allowed.Value().diagnostics.overflowedClusterCount == 1);
    CHECK(references[0] == 0);
    clusters[0].planes[0].offset += 1;
    CHECK(PrepareClusteredLightLists(grid, lights, clusters, budget, membership, references).HasError());
}

TEST_CASE("Cluster preparation accepts all 4096 canonical lights instead of a viewport sixteen light prefix",
          "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 1;
    const LightCullingBudget budget{.maximumLights = 4096, .maximumClusters = 1, .referencesPerCluster = 4096};
    std::array<LightCluster, 1> clusters;
    const auto grid = Build(descriptor, clusters, budget);
    std::array<IdentifiedRenderLight, 4096> lights;
    for (unsigned index = 0; index < lights.size(); ++index)
        lights[index] = Light(index + 1, {}, RenderLightKind::Directional);
    std::array<LightClusterMembership, 1> membership;
    std::array<std::uint32_t, 4096> references;
    const auto prepared = PrepareClusteredLightLists(grid, lights, clusters, budget, membership, references);
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().diagnostics.lightCount == 4096);
    CHECK(prepared.Value().diagnostics.retainedReferenceCount == 4096);
    CHECK(references.back() == 4095);
}

TEST_CASE("Cancelled cluster candidates preserve previously published immutable list storage", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 1;
    std::array<LightCluster, 1> clusters;
    const auto grid = Build(descriptor, clusters);
    const std::array lights{Light(1, {}, RenderLightKind::Directional)};
    const LightCullingBudget budget{.maximumLights = 1, .maximumClusters = 1, .referencesPerCluster = 1};
    std::array<LightClusterMembership, 1> readyMembership, stagingMembership;
    std::array<std::uint32_t, 1> readyReferences, stagingReferences{99};
    const auto ready = PrepareClusteredLightLists(grid, lights, clusters, budget, readyMembership, readyReferences);
    REQUIRE(ready.HasValue());
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    const auto candidate =
        PrepareClusteredLightLists(grid, lights, clusters, budget, stagingMembership, stagingReferences, cancelled.Token());
    REQUIRE(candidate.HasError());
    CHECK(candidate.ErrorValue().code.Value() == LightCullingErrors::Cancelled.code.Value());
    CHECK(stagingReferences[0] == 99);
    CHECK(ready.Value().ReferencesForCluster(0).Value()[0] == 0);
}

TEST_CASE("CPU forward fallback culls the full table per actual draw and returns global source indices", "[renderer][light][forward]") {
    std::array<IdentifiedRenderLight, 32> lights;
    for (unsigned index = 0; index < lights.size(); ++index)
        lights[index] = Light(index + 1, {index < 16 ? 0.0F : 100.0F, 0, 0});
    const LightCullingBudget budget{.maximumLights = 32, .maximumClusters = 1, .referencesPerCluster = 16};
    std::array<RenderLight, 16> output;
    const auto left = PrepareDrawForwardLights(lights, {{-1, -1, -1}, {1, 1, 1}}, budget, output);
    REQUIRE(left.HasValue());
    CHECK(left.Value().count == 16);
    CHECK(left.Value().sourceIndices[0] == 0);
    const auto right = PrepareDrawForwardLights(lights, {{99, -1, -1}, {101, 1, 1}}, budget, output);
    REQUIRE(right.HasValue());
    CHECK(right.Value().count == 16);
    CHECK(right.Value().sourceIndices[0] == 16);
    CHECK(right.Value().sourceIndices[15] == 31);
    CHECK(output[0].stableIdentity == 17);
}

TEST_CASE("CPU forward required overflow cancellation and invalid identity preserve prior draw outputs", "[renderer][light][forward]") {
    std::array<IdentifiedRenderLight, 17> lights;
    for (unsigned index = 0; index < lights.size(); ++index)
        lights[index] = Light(index + 1, {}, RenderLightKind::Directional);
    LightCullingBudget budget{.maximumLights = 17, .maximumClusters = 1, .referencesPerCluster = 16};
    std::array<RenderLight, 16> output;
    output[0].intensity = 123;
    const auto required = PrepareDrawForwardLights(lights, {{-1, -1, -1}, {1, 1, 1}}, budget, output);
    REQUIRE(required.HasError());
    CHECK(required.ErrorValue().code.Value() == LightCullingErrors::Coverage.code.Value());
    CHECK(output[0].intensity == 123);
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    CHECK(PrepareDrawForwardLights(lights, {{-1, -1, -1}, {1, 1, 1}}, budget, output, cancelled.Token()).HasError());
    CHECK(output[0].intensity == 123);
    budget.requireCompleteCoverage = false;
    const auto optional = PrepareDrawForwardLights(lights, {{-1, -1, -1}, {1, 1, 1}}, budget, output);
    REQUIRE(optional.HasValue());
    CHECK(optional.Value().count == 16);
    CHECK(optional.Value().omitted == 1);
    lights[0].light.stableIdentity = 99;
    CHECK(PrepareDrawForwardLights(lights, {{-1, -1, -1}, {1, 1, 1}}, budget, output).HasError());
}

TEST_CASE("Native clustered selection requires real specialized culling and material consumer predicates", "[renderer][light][recipe]") {
    LightPreparationSupport support{true, true, true, true};
    const auto native = SelectLightPreparation(LightPreparationMode::ClusteredForwardNative, LightPreparationFallback::Forbidden, support);
    REQUIRE(native.HasValue());
    CHECK(native.Value().mode == LightPreparationMode::ClusteredForwardNative);
    const auto missing = GENERATE(0, 1, 2, 3);
    if (missing == 0)
        support.nativeCulling = false;
    if (missing == 1)
        support.storageRaster = false;
    if (missing == 2)
        support.sameGraphConsumer = false;
    if (missing == 3)
        support.depthPrepass = false;
    CHECK(SelectLightPreparation(LightPreparationMode::ClusteredForwardNative, LightPreparationFallback::Forbidden, support).HasError());
    const auto fallback =
        SelectLightPreparation(LightPreparationMode::ClusteredForwardNative, LightPreparationFallback::AllowBaselineForwardCpu, support);
    REQUIRE(fallback.HasValue());
    CHECK(fallback.Value().mode == LightPreparationMode::BaselineForwardCpu);
    CHECK(fallback.Value().missingPredicates == (1U << missing));
}
