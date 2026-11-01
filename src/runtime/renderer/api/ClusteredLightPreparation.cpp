#include "ClusteredLightGridInternal.h"

#include <algorithm>
#include <format>

namespace Horo::Render {
    namespace {
        bool MatchingIdentities(std::span<const IdentifiedRenderLight> lights) noexcept {
            return std::ranges::all_of(lights, [](const auto &entry) {
                return entry.light.stableIdentity == 0 || entry.light.stableIdentity == entry.identity.value;
            });
        }

        ClusteredLightDiagnostics Diagnostics(std::span<const LightClusterMembership> membership, std::uint32_t lightCount) noexcept {
            ClusteredLightDiagnostics diagnostics{.lightCount = lightCount, .clusterCount = static_cast<std::uint32_t>(membership.size())};
            for (const auto &record : membership) {
                diagnostics.retainedReferenceCount += record.count;
                diagnostics.omittedReferenceCount += record.omitted;
                diagnostics.maximumRetainedPerCluster = std::max(diagnostics.maximumRetainedPerCluster, record.count);
                if (record.omitted != 0)
                    ++diagnostics.overflowedClusterCount;
            }
            diagnostics.completeCoverage = diagnostics.omittedReferenceCount == 0;
            return diagnostics;
        }

        LightCluster DrawVolume(const Math::Aabb &bounds) noexcept {
            LightCluster cluster;
            cluster.planes = {LightClusterPlane{{1, 0, 0}, -bounds.minimum.x}, LightClusterPlane{{-1, 0, 0}, bounds.maximum.x},
                              LightClusterPlane{{0, 1, 0}, -bounds.minimum.y}, LightClusterPlane{{0, -1, 0}, bounds.maximum.y},
                              LightClusterPlane{{0, 0, 1}, -bounds.minimum.z}, LightClusterPlane{{0, 0, -1}, bounds.maximum.z}};
            return cluster;
        }

        std::uint8_t Missing(const LightPreparationSupport &support) noexcept {
            std::uint8_t missing{};
            if (!support.nativeCulling)
                missing |= static_cast<std::uint8_t>(LightPreparationMissing::NativeCulling);
            if (!support.storageRaster)
                missing |= static_cast<std::uint8_t>(LightPreparationMissing::StorageRaster);
            if (!support.sameGraphConsumer)
                missing |= static_cast<std::uint8_t>(LightPreparationMissing::SameGraphConsumer);
            if (!support.depthPrepass)
                missing |= static_cast<std::uint8_t>(LightPreparationMissing::DepthPrepass);
            return missing;
        }
    }  // namespace

    Result<PreparedClusteredLights> PrepareClusteredLightLists(const ClusteredLightGrid &grid,
                                                               std::span<const IdentifiedRenderLight> lights,
                                                               std::span<const LightCluster> clusters, const LightCullingBudget &budget,
                                                               std::span<LightClusterMembership> membership,
                                                               std::span<std::uint32_t> references, const CancellationToken &cancellation) {
        using Outcome = Result<PreparedClusteredLights>;
        if (!grid.IsValid() || clusters.size() != grid.clusterCount || !MatchingIdentities(lights))
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (ClusteredLightDetail::Signature(clusters) != grid.clusterSignature)
            return Outcome::Failure(
                MakeError(LightCullingErrors::InvalidInput, "Cluster planes do not belong to this prepared camera grid."));
        auto diagnosticBudget = budget;
        diagnosticBudget.requireCompleteCoverage = false;
        if (const auto culled = CullLightsCpu(lights, clusters, diagnosticBudget, membership, references, cancellation); culled.HasError())
            return Outcome::Failure(culled.ErrorValue());
        const auto diagnostics = Diagnostics(membership.first(grid.clusterCount), static_cast<std::uint32_t>(lights.size()));
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(LightCullingErrors::Cancelled));
        if (budget.requireCompleteCoverage && !diagnostics.completeCoverage)
            return Outcome::Failure(MakeError(LightCullingErrors::Coverage,
                                              std::format("{} clusters omit {} light references.", diagnostics.overflowedClusterCount,
                                                          diagnostics.omittedReferenceCount)));
        return Outcome::Success({grid, budget, lights, clusters, membership.first(grid.clusterCount),
                                 references.first(static_cast<std::size_t>(grid.clusterCount) * budget.referencesPerCluster), diagnostics});
    }

    Result<std::span<const std::uint32_t>> PreparedClusteredLights::ReferencesForCluster(std::uint32_t index) const {
        using Outcome = Result<std::span<const std::uint32_t>>;
        if (index >= membership.size())
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto &record = membership[index];
        if (record.offset > references.size() || record.count > references.size() - record.offset)
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto selected = references.subspan(record.offset, record.count);
        if (std::ranges::any_of(selected, [&](auto source) {
            return source >= lights.size();
        }))
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        return Outcome::Success(selected);
    }

    Result<ForwardDrawLightSelection> PrepareDrawForwardLights(std::span<const IdentifiedRenderLight> lights, const Math::Aabb &worldBounds,
                                                               const LightCullingBudget &budget, std::span<RenderLight> output,
                                                               const CancellationToken &cancellation) {
        using Outcome = Result<ForwardDrawLightSelection>;
        if (!worldBounds.IsValid() || !MatchingIdentities(lights))
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (budget.referencesPerCluster > MaximumForwardLights)
            return Outcome::Failure(MakeError(LightCullingErrors::Unsupported));
        if (output.size() < budget.referencesPerCluster)
            return Outcome::Failure(MakeError(LightCullingErrors::Capacity));
        ForwardDrawLightSelection selection;
        LightClusterMembership membership;
        const auto volume = DrawVolume(worldBounds);
        if (const auto culled =
                CullLightsCpu(lights, std::span{&volume, 1}, budget, std::span{&membership, 1}, selection.sourceIndices, cancellation);
            culled.HasError())
            return Outcome::Failure(culled.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(LightCullingErrors::Cancelled));
        selection.count = membership.count;
        selection.omitted = membership.omitted;
        for (std::uint32_t index = 0; index < selection.count; ++index)
            output[index] = lights[selection.sourceIndices[index]].light;
        return Outcome::Success(selection);
    }

    Result<LightPreparationSelection> SelectLightPreparation(LightPreparationMode requested, LightPreparationFallback fallback,
                                                             const LightPreparationSupport &support) {
        using Outcome = Result<LightPreparationSelection>;
        if (fallback != LightPreparationFallback::Forbidden && fallback != LightPreparationFallback::AllowBaselineForwardCpu)
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (requested == LightPreparationMode::BaselineForwardCpu)
            return Outcome::Success({requested, 0});
        if (requested != LightPreparationMode::ClusteredForwardNative)
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto missing = Missing(support);
        if (missing == 0)
            return Outcome::Success({requested, 0});
        if (fallback == LightPreparationFallback::Forbidden)
            return Outcome::Failure(MakeError(LightCullingErrors::Unsupported));
        return Outcome::Success({LightPreparationMode::BaselineForwardCpu, missing});
    }
}  // namespace Horo::Render
