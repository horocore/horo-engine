#include "Horo/Runtime/Render/LightFrameUpload.h"

namespace Horo::Render {
    namespace {
        /** @brief Decode the fixed ABI without interpreting integer identity or kind as floating point. */
        RenderLight Decode(const PackedRenderLight &packed) {
            return {.kind = static_cast<RenderLightKind>(packed.kind),
                    .position = {packed.positionRange[0], packed.positionRange[1], packed.positionRange[2]},
                    .direction = {packed.direction[0], packed.direction[1], packed.direction[2]},
                    .color = {packed.colorIntensity[0], packed.colorIntensity[1], packed.colorIntensity[2]},
                    .intensity = packed.colorIntensity[3],
                    .range = packed.positionRange[3],
                    .innerConeCosine = packed.cones[0],
                    .outerConeCosine = packed.cones[1]};
        }

        /** @brief Restore six CPU-reference half-spaces from the documented ABI. */
        LightCluster Decode(const PackedLightCluster &packed) {
            LightCluster cluster;
            for (std::size_t index = 0; index < 6; ++index)
                cluster.planes[index] = {{packed.planes[index][0], packed.planes[index][1], packed.planes[index][2]},
                                         packed.planes[index][3]};
            return cluster;
        }
    }  // namespace

    /** @copydoc ValidateLightFrameUpdate */
    Result<void> ValidateLightFrameUpdate(const LightFrameUpdate &update) {
        if (update.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
        if (!update.budget.IsValid() || update.revision == 0 || update.clusters.empty())
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (update.lights.size() > update.budget.maximumLights || update.clusters.size() > update.budget.maximumClusters)
            return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
        std::uint64_t previous{};
        for (const auto &packed : update.lights) {
            if (update.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
            const std::uint64_t identity = (std::uint64_t{packed.identityHigh} << 32U) | packed.identityLow;
            if (identity <= previous || packed.kind > static_cast<std::uint32_t>(RenderLightKind::Spot) || !Decode(packed).IsValid())
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            previous = identity;
        }
        for (const auto &packed : update.clusters) {
            if (update.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
            const auto cluster = Decode(packed);
            if (!cluster.IsValid())
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            if (!update.budget.requireCompleteCoverage || update.budget.referencesPerCluster >= update.lights.size())
                continue;
            std::size_t count{};
            for (const auto &light : update.lights) {
                if (!LightIntersectsCluster(Decode(light), cluster))
                    continue;
                ++count;
                if (count > update.budget.referencesPerCluster)
                    return Result<void>::Failure(MakeError(LightCullingErrors::Coverage));
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
