#include "ClusteredLightTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Render::ClusteredLightTests;

TEST_CASE("Camera cluster grid and actual projected pixel depth select the same three dimensional cells", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.depthRange = GENERATE(Math::ClipDepthRange::ZeroToOne, Math::ClipDepthRange::NegativeOneToOne);
    descriptor.depthPartition = GENERATE(LightClusterDepthPartition::Linear, LightClusterDepthPartition::Logarithmic);
    descriptor.camera.projection.kind = GENERATE(RenderProjectionKind::Perspective, RenderProjectionKind::Orthographic);
    std::array<LightCluster, 24> clusters;
    const auto grid = Build(descriptor, clusters);
    REQUIRE(grid.IsValid());
    CHECK(grid.clusterCount == 24);
    for (unsigned z = 0; z < descriptor.z; ++z)
        for (unsigned y = 0; y < descriptor.y; ++y)
            for (unsigned x = 0; x < descriptor.x; ++x) {
                const auto point = CellPoint(descriptor, x, y, z);
                const auto clip = Math::TransformHomogeneous(grid.viewProjection, {point.x, point.y, point.z, 1});
                const auto pixelX = (clip.x / clip.w + 1) * 0.5F * descriptor.extent.width;
                const auto pixelY = (1 - clip.y / clip.w) * 0.5F * descriptor.extent.height;
                const float depth =
                    descriptor.depthRange == Math::ClipDepthRange::ZeroToOne ? clip.z / clip.w : (clip.z / clip.w + 1) * 0.5F;
                const auto mapped = MapClusteredLightPixel(grid, pixelX, pixelY, depth);
                REQUIRE(mapped.HasValue());
                REQUIRE(mapped.Value());
                const auto index = x + descriptor.x * (y + descriptor.y * z);
                CHECK(*mapped.Value() == index);
                const auto forward = Math::Normalize(descriptor.camera.target - descriptor.camera.position);
                const float actualDepth = Math::Dot(point - descriptor.camera.position, forward);
                const auto geometric = MapClusteredLightViewDepth(grid, pixelX, pixelY, actualDepth);
                REQUIRE(geometric.HasValue());
                CHECK(geometric.Value() == mapped.Value());
                CHECK(LightIntersectsCluster(Light(1, point).light, clusters[index]));
            }
}

TEST_CASE("Camera grid depth has distinct near and far slices and exclusive right bottom pixel edges", "[renderer][light][cluster]") {
    const auto descriptor = Descriptor();
    std::array<LightCluster, 24> clusters;
    const auto grid = Build(descriptor, clusters);
    CHECK(MapClusteredLightPixel(grid, 0, 0, 0).Value() == 0);
    CHECK(MapClusteredLightPixel(grid, 639, 479, 1).Value() == 23);
    CHECK_FALSE(MapClusteredLightPixel(grid, 640, 0, 0.5F).Value());
    CHECK_FALSE(MapClusteredLightPixel(grid, 0, 480, 0.5F).Value());
    CHECK_FALSE(MapClusteredLightPixel(grid, -0.01F, 0, 0.5F).Value());
    CHECK_FALSE(MapClusteredLightPixel(grid, 0, 0, -0.01F).Value());
    CHECK(MapClusteredLightPixel(grid, 0, 0, std::numeric_limits<float>::quiet_NaN()).HasError());
}

TEST_CASE("Camera grid rejects malformed projection domains dimensions and insufficient storage", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5);
    if (mutation == 0)
        descriptor.z = 0;
    if (mutation == 1)
        descriptor.x = 4096;
    if (mutation == 2)
        descriptor.identity.sceneDomain = 0;
    if (mutation == 3)
        descriptor.depthPartition = static_cast<LightClusterDepthPartition>(255);
    if (mutation == 4)
        descriptor.depthRange = static_cast<Math::ClipDepthRange>(255);
    if (mutation == 5)
        descriptor.camera.up = descriptor.camera.target - descriptor.camera.position;
    std::array<LightCluster, 24> clusters;
    CHECK(BuildClusteredLightGrid(descriptor, {}, clusters).HasError());
    CHECK(BuildClusteredLightGrid(Descriptor(), {}, std::span(clusters).first(23)).HasError());
}

TEST_CASE("Camera grid supports the complete existing 4096 cluster envelope and cooperative cancellation", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.x = 16;
    descriptor.y = 16;
    descriptor.z = 16;
    const LightCullingBudget budget{.maximumLights = 1, .maximumClusters = 4096, .referencesPerCluster = 1};
    std::array<LightCluster, 4096> clusters;
    CHECK(Build(descriptor, clusters, budget).clusterCount == 4096);
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    const auto failed = BuildClusteredLightGrid(descriptor, budget, clusters, cancelled.Token());
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == LightCullingErrors::Cancelled.code.Value());
}

TEST_CASE("Geometric cluster mapping retains distant geometry when perspective window depth loses distance", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.camera = {};
    descriptor.camera.projection.kind = GENERATE(RenderProjectionKind::Perspective, RenderProjectionKind::Orthographic);
    descriptor.camera.projection.nearPlane = 0.1F;
    descriptor.camera.projection.farPlane = 100000000.0F;
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 8;
    std::array<LightCluster, 8> clusters;
    const auto grid = Build(descriptor, clusters);
    const Math::Vec3 point{0, 0, -3000000};
    const auto forward = Math::Normalize(descriptor.camera.target - descriptor.camera.position);
    const auto depth = Math::Dot(point - descriptor.camera.position, forward);
    const auto clip = Math::TransformHomogeneous(grid.viewProjection, {point.x, point.y, point.z, 1});
    const auto pixelX = (clip.x / clip.w + 1) * 0.5F * descriptor.extent.width;
    const auto pixelY = (1 - clip.y / clip.w) * 0.5F * descriptor.extent.height;
    const auto geometric = MapClusteredLightViewDepth(grid, pixelX, pixelY, depth);
    REQUIRE(geometric.HasValue());
    REQUIRE(geometric.Value());
    CHECK(*geometric.Value() == 6);
    auto localLight = Light(1, point).light;
    localLight.range = 1;
    CHECK(LightIntersectsCluster(localLight, clusters[*geometric.Value()]));
    CHECK_FALSE(LightIntersectsCluster(localLight, clusters[7]));
    const auto quantized = MapClusteredLightPixel(grid, pixelX, pixelY, clip.z / clip.w);
    REQUIRE(quantized.HasValue());
    if (descriptor.camera.projection.kind == RenderProjectionKind::Perspective) {
        CHECK(clip.z / clip.w == 1);
        CHECK(quantized.Value() == 7);
    } else {
        CHECK(quantized.Value() == geometric.Value());
    }
}

TEST_CASE("Geometric cluster mapping validates actual distance and finite exclusive viewport bounds", "[renderer][light][cluster]") {
    const auto descriptor = Descriptor();
    std::array<LightCluster, 24> clusters;
    const auto grid = Build(descriptor, clusters);
    CHECK(MapClusteredLightViewDepth(grid, 0, 0, 1).Value() == 0);
    CHECK(MapClusteredLightViewDepth(grid, 639, 479, 81).Value() == 23);
    CHECK_FALSE(MapClusteredLightViewDepth(grid, 0, 0, 0.5F).Value());
    CHECK_FALSE(MapClusteredLightViewDepth(grid, 0, 0, 82).Value());
    CHECK_FALSE(MapClusteredLightViewDepth(grid, 640, 0, 10).Value());
    CHECK_FALSE(MapClusteredLightViewDepth(grid, 0, 480, 10).Value());
    CHECK(MapClusteredLightViewDepth(grid, 0, 0, 0).HasError());
    CHECK(MapClusteredLightViewDepth(grid, 0, 0, -1).HasError());
    CHECK(MapClusteredLightViewDepth(grid, 0, 0, std::numeric_limits<float>::quiet_NaN()).HasError());
}

TEST_CASE("Geometric cluster mapping admits huge finite depth ratios with actual conservative volumes", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.camera = {};
    descriptor.camera.projection.kind = GENERATE(RenderProjectionKind::Perspective, RenderProjectionKind::Orthographic);
    descriptor.camera.projection.nearPlane = 0.0000000001F;
    descriptor.camera.projection.farPlane = 1.0e30F;
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 8;
    std::array<LightCluster, 8> clusters;
    const auto grid = Build(descriptor, clusters);
    const Math::Vec3 point{0, 0, -3.0e12F};
    const auto forward = Math::Normalize(descriptor.camera.target - descriptor.camera.position);
    const auto actualDepth = Math::Dot(point - descriptor.camera.position, forward);
    const auto mapped = MapClusteredLightViewDepth(grid, 320, 240, actualDepth);
    REQUIRE(mapped.HasValue());
    REQUIRE(mapped.Value());
    CHECK(*mapped.Value() == 4);
    auto light = Light(1, point).light;
    light.range = 1.0e8F;
    CHECK(LightIntersectsCluster(light, clusters[*mapped.Value()]));
    CHECK_FALSE(LightIntersectsCluster(light, clusters[7]));
    CHECK(MapClusteredLightViewDepth(grid, 320, 240, descriptor.camera.projection.nearPlane).Value() == 0);
    CHECK(MapClusteredLightViewDepth(grid, 320, 240, descriptor.camera.projection.farPlane).Value() == 7);
}

TEST_CASE("Geometric cluster mapping preserves close ratio boundaries at large absolute distances", "[renderer][light][cluster]") {
    auto descriptor = Descriptor();
    descriptor.camera = {};
    descriptor.camera.projection.kind = GENERATE(RenderProjectionKind::Perspective, RenderProjectionKind::Orthographic);
    descriptor.camera.projection.nearPlane = 100000000.0F;
    descriptor.camera.projection.farPlane = 100000016.0F;
    descriptor.x = 1;
    descriptor.y = 1;
    descriptor.z = 2;
    std::array<LightCluster, 2> clusters;
    const auto grid = Build(descriptor, clusters);
    CHECK(MapClusteredLightViewDepth(grid, 320, 240, 100000000.0F).Value() == 0);
    const Math::Vec3 point{0, 0, -100000008.0F};
    const auto forward = Math::Normalize(descriptor.camera.target - descriptor.camera.position);
    const auto depth = Math::Dot(point - descriptor.camera.position, forward);
    const auto boundary = MapClusteredLightViewDepth(grid, 320, 240, depth);
    REQUIRE(boundary.HasValue());
    REQUIRE(boundary.Value());
    CHECK(*boundary.Value() == 1);
    auto light = Light(1, point).light;
    light.range = 1;
    CHECK(LightIntersectsCluster(light, clusters[*boundary.Value()]));
    CHECK(MapClusteredLightViewDepth(grid, 320, 240, 100000016.0F).Value() == 1);
}
