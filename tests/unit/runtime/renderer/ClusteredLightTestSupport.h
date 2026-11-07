#pragma once
#include "Horo/Runtime/Render/ClusteredLightGrid.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace Horo::Render::ClusteredLightTests {
    inline ClusteredLightGridDescriptor Descriptor() {
        return {.identity = {{17}, 41, 1, 3, 5},
                .camera = {.position = {2, 3, 4}, .target = {1, 2, 2}, .projection = {.nearPlane = 1, .farPlane = 81}},
                .extent = {640, 480},
                .x = 3,
                .y = 2,
                .z = 4};
    }

    inline ClusteredLightGrid Build(const ClusteredLightGridDescriptor &descriptor, std::span<LightCluster> clusters,
                                    const LightCullingBudget &budget = {}) {
        const auto built = BuildClusteredLightGrid(descriptor, budget, clusters);
        REQUIRE(built.HasValue());
        return built.Value();
    }

    inline Math::Vec3 CellPoint(const ClusteredLightGridDescriptor &descriptor, unsigned x, unsigned y, unsigned z) {
        const auto forward = Math::Normalize(descriptor.camera.target - descriptor.camera.position);
        const auto right = Math::Normalize(Math::Cross(forward, descriptor.camera.up));
        const auto up = Math::Cross(right, forward);
        const float distance =
            descriptor.depthPartition == LightClusterDepthPartition::Logarithmic ? std::pow(3.0F, z + 0.5F) : 11.0F + 20.0F * z;
        const float halfHeight = descriptor.camera.projection.kind == RenderProjectionKind::Perspective
                                     ? std::tan(descriptor.camera.projection.verticalFovRadians * 0.5F) * distance
                                     : descriptor.camera.projection.orthographicHeight * 0.5F;
        const float horizontal = -1 + 2 * (x + 0.5F) / descriptor.x, vertical = 1 - 2 * (y + 0.5F) / descriptor.y;
        return descriptor.camera.position + forward * distance +
               right * (horizontal * halfHeight * descriptor.extent.width / descriptor.extent.height) + up * (vertical * halfHeight);
    }

    inline IdentifiedRenderLight Light(std::uint64_t identity, Math::Vec3 position = {}, RenderLightKind kind = RenderLightKind::Point) {
        IdentifiedRenderLight entry{{identity}, {.kind = kind, .position = position, .range = 0.01F}};
        entry.light.stableIdentity = identity;
        return entry;
    }
}  // namespace Horo::Render::ClusteredLightTests
