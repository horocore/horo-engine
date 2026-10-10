#include "Horo/Runtime/Render/LightFrameLayout.h"

#include <algorithm>

namespace Horo::Render {
    /** @copydoc PackLightFrame */
    Result<void> PackLightFrame(const std::span<const IdentifiedRenderLight> lights, const std::span<const LightCluster> clusters,
                                const std::span<PackedRenderLight> packedLights, const std::span<PackedLightCluster> packedClusters) {
        if (lights.size() > LightCullingBudget::HardMaximumLights || clusters.size() > LightCullingBudget::HardMaximumClusters ||
            packedLights.size() < lights.size() || packedClusters.size() < clusters.size())
            return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
        RenderLightIdentity previous;
        for (const auto &entry : lights) {
            if (entry.identity.value == 0 || entry.identity <= previous || !entry.light.IsValid())
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            previous = entry.identity;
        }
        if (!std::ranges::all_of(clusters, &LightCluster::IsValid))
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        for (std::size_t index = 0; index < lights.size(); ++index) {
            const auto &entry = lights[index];
            const auto &light = entry.light;
            packedLights[index] = {.positionRange = {light.position.x, light.position.y, light.position.z, light.range},
                                   .direction = {light.direction.x, light.direction.y, light.direction.z},
                                   .kind = static_cast<std::uint32_t>(light.kind),
                                   .colorIntensity = {light.color.x, light.color.y, light.color.z, light.intensity},
                                   .cones = {light.innerConeCosine, light.outerConeCosine},
                                   .identityLow = static_cast<std::uint32_t>(entry.identity.value),
                                   .identityHigh = static_cast<std::uint32_t>(entry.identity.value >> 32U)};
        }
        for (std::size_t index = 0; index < clusters.size(); ++index) {
            for (std::size_t planeIndex = 0; planeIndex < 6; ++planeIndex) {
                const auto &plane = clusters[index].planes[planeIndex];
                packedClusters[index].planes[planeIndex] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.offset};
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
