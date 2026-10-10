#include "Horo/Runtime/Render/LightCulling.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    namespace LightCullingErrors {
        const ErrorCodeDescriptor InvalidInput{ErrorDomainId{"horo.render"}, ErrorCode{"render.light.invalid"}, ErrorSeverity::Error,
                                               "Light preparation input is invalid.",
                                               "Supply finite lights, normalized cluster planes and unique ordered identities."};
        const ErrorCodeDescriptor Capacity{ErrorDomainId{"horo.render"}, ErrorCode{"render.light.capacity"}, ErrorSeverity::Error,
                                           "Light preparation exceeds its admitted bound.",
                                           "Supply sufficient scratch storage within the cooked light budget."};
        const ErrorCodeDescriptor Coverage{ErrorDomainId{"horo.render"}, ErrorCode{"render.light.coverage"}, ErrorSeverity::Error,
                                           "Required complete light coverage cannot be represented.",
                                           "Increase the admitted cluster reference budget before activation."};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"horo.render"}, ErrorCode{"render.light.cancelled"}, ErrorSeverity::Error,
                                            "Light preparation was cancelled.", "Discard unpublished scratch and end the host operation."};
        const ErrorCodeDescriptor Unsupported{ErrorDomainId{"horo.render"}, ErrorCode{"render.light.unsupported"}, ErrorSeverity::Error,
                                              "The selected light preparation implementation is unavailable.",
                                              "Admit an explicitly permitted recipe with effective implementation support."};
        const ErrorCodeDescriptor Pending{ErrorDomainId{"horo.render"},
                                          ErrorCode{"render.light.pending"},
                                          ErrorSeverity::Info,
                                          "The light frame slot is still in native use.",
                                          "Retry another admitted slot or poll this slot at a later owner safe point.",
                                          true};
    }  // namespace LightCullingErrors

    /** @copydoc LightClusterPlane::IsValid */
    bool LightClusterPlane::IsValid() const noexcept {
        return Math::IsFinite(normal) && std::isfinite(offset) && Math::NearlyEqual(Math::Length(normal), 1.0F, 0.001F);
    }

    /** @copydoc LightCluster::IsValid */
    bool LightCluster::IsValid() const noexcept {
        return std::ranges::all_of(planes, &LightClusterPlane::IsValid);
    }

    /** @copydoc LightIntersectsCluster */
    bool LightIntersectsCluster(const RenderLight &light, const LightCluster &cluster) noexcept {
        if (light.intensity == 0.0F)
            return false;
        if (light.kind == RenderLightKind::Directional)
            return true;
        if (light.range == 0.0F)
            return false;
        for (const auto &plane : cluster.planes) {
            // Range is authored in world units, independent of object scale.
            const float distance = Math::Dot(plane.normal, light.position) + plane.offset;
            if (distance < -light.range * Math::Length(plane.normal))
                return false;
        }
        return true;
    }

    /** @copydoc CullLightsCpu */
    Result<void> CullLightsCpu(const std::span<const IdentifiedRenderLight> lights, const std::span<const LightCluster> clusters,
                               const LightCullingBudget &budget, const std::span<LightClusterMembership> membership,
                               const std::span<std::uint32_t> references, const CancellationToken &cancellation) {
        if (!budget.IsValid())
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (lights.size() > budget.maximumLights || clusters.size() > budget.maximumClusters || membership.size() < clusters.size() ||
            references.size() < clusters.size() * budget.referencesPerCluster)
            return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
        RenderLightIdentity previous;
        for (const auto &entry : lights) {
            if (entry.identity.value == 0 || entry.identity <= previous || !entry.light.IsValid())
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            previous = entry.identity;
        }
        if (!std::ranges::all_of(clusters, &LightCluster::IsValid))
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
        for (std::size_t clusterIndex = 0; clusterIndex < clusters.size(); ++clusterIndex) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
            LightClusterMembership output{.offset = static_cast<std::uint32_t>(clusterIndex) * budget.referencesPerCluster};
            for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
                if (!LightIntersectsCluster(lights[lightIndex].light, clusters[clusterIndex]))
                    continue;
                if (output.count == budget.referencesPerCluster)
                    ++output.omitted;
                else
                    references[output.offset + output.count++] = static_cast<std::uint32_t>(lightIndex);
            }
            if (output.omitted != 0 && budget.requireCompleteCoverage)
                return Result<void>::Failure(MakeError(LightCullingErrors::Coverage));
            membership[clusterIndex] = output;
        }
        return Result<void>::Success();
    }

    /** @copydoc PrepareForwardLights */
    Result<ForwardLightSelection> PrepareForwardLights(const std::span<const IdentifiedRenderLight> lights, const LightCluster &volume,
                                                       const LightCullingBudget &budget, const std::span<RenderLight> output,
                                                       const CancellationToken &cancellation) {
        using Selection = Result<ForwardLightSelection>;
        if (budget.referencesPerCluster > MaximumForwardLights)
            return Selection::Failure(MakeError(LightCullingErrors::Unsupported));
        if (output.size() < budget.referencesPerCluster)
            return Selection::Failure(MakeError(LightCullingErrors::Capacity));
        std::array<std::uint32_t, MaximumForwardLights> indices{};
        LightClusterMembership membership;
        if (const auto culled = CullLightsCpu(lights, std::span{&volume, 1}, budget, std::span{&membership, 1}, indices, cancellation);
            culled.HasError())
            return Selection::Failure(culled.ErrorValue());
        for (std::size_t index = 0; index < membership.count; ++index)
            output[index] = lights[indices[index]].light;
        return Selection::Success({membership.count, membership.omitted});
    }
}  // namespace Horo::Render
