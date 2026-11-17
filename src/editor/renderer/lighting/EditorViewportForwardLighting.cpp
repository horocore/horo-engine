#include "EditorViewportForwardLighting.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        bool KnownProfileEnums(const EditorViewportLightingProfile &profile) noexcept {
            const bool requested = profile.requested == Render::LightPreparationMode::BaselineForwardCpu ||
                                   profile.requested == Render::LightPreparationMode::ClusteredForwardNative;
            const bool fallback = profile.fallback == Render::LightPreparationFallback::Forbidden ||
                                  profile.fallback == Render::LightPreparationFallback::AllowBaselineForwardCpu;
            return requested && fallback;
        }

        bool CpuSelectionMatches(const EditorViewportLightingProfile &profile,
                                 const Render::LightPreparationSelection &selection) noexcept {
            if (selection.mode != Render::LightPreparationMode::BaselineForwardCpu)
                return false;
            if (profile.requested == Render::LightPreparationMode::BaselineForwardCpu)
                return selection.missingPredicates == 0;
            return profile.fallback == Render::LightPreparationFallback::AllowBaselineForwardCpu && selection.missingPredicates != 0 &&
                   selection.missingPredicates <= 15;
        }
    }  // namespace

    Result<void> EditorViewportForwardLighting::Configure(const EditorViewportLightingProfile &profile,
                                                          const Render::LightPreparationSelection &selection) {
        if (!profile.IsValid() || !KnownProfileEnums(profile))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        if (!CpuSelectionMatches(profile, selection))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Unsupported));
        profile_ = profile;
        budget_ = profile.budget;
        budget_.referencesPerCluster =
            std::min(profile.budget.referencesPerCluster, static_cast<std::uint32_t>(Render::MaximumForwardLights));
        diagnostics_.selection = selection;
        admitted_ = true;
        ready_ = false;
        return Result<void>::Success();
    }

    Result<void> EditorViewportForwardLighting::CopyCanonicalLights(const Render::RenderSceneView &scene) {
        if (scene.lights.size() > budget_.maximumLights)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
        if ((!scene.lights.empty() || !scene.instances.empty()) && (scene.lightSceneDomain == 0 || scene.lightSnapshotRevision == 0))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        count_ = scene.lights.size();
        for (std::size_t index = 0; index < count_; ++index)
            lights_[index] = {{scene.lights[index].stableIdentity}, scene.lights[index]};
        auto lights = std::span{lights_}.first(count_);
        std::ranges::sort(lights, {}, &Render::IdentifiedRenderLight::identity);
        std::uint64_t previous{};
        for (const auto &light : lights) {
            if (light.identity.value <= previous || !light.light.IsValid())
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            previous = light.identity.value;
        }
        domain_ = scene.lightSceneDomain;
        revision_ = scene.lightSnapshotRevision;
        return Result<void>::Success();
    }

    Result<void> EditorViewportForwardLighting::Prepare(const Render::RenderSceneView &scene, const CancellationToken &cancellation) {
        ready_ = false;
        if (!admitted_)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Unsupported));
        if (const auto copied = CopyCanonicalLights(scene); copied.HasError())
            return copied;
        if (scene.instances.size() > profile_.maximumForwardLightTests / (count_ + 1))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
        EditorViewportLightingDiagnostics candidate{.selection = diagnostics_.selection,
                                                    .sourceLightCount = static_cast<std::uint32_t>(count_)};
        for (const auto &instance : scene.instances) {
            std::array<Render::RenderLight, Render::MaximumForwardLights> staging;
            const auto selection = SelectInstance(instance, staging, cancellation);
            if (selection.HasError())
                return Result<void>::Failure(selection.ErrorValue());
            candidate.shadedReferences += selection.Value().count;
            candidate.omittedReferences += selection.Value().omitted;
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Cancelled));
        diagnostics_ = candidate;
        ready_ = true;
        return Result<void>::Success();
    }

    bool EditorViewportForwardLighting::MatchesScene(const Render::RenderSceneView &scene) const noexcept {
        return ready_ && scene.lightSceneDomain == domain_ && scene.lightSnapshotRevision == revision_ && scene.lights.size() == count_;
    }

    Result<Render::ForwardDrawLightSelection> EditorViewportForwardLighting::SelectInstance(
        const Render::RenderStaticMeshInstance &instance, const std::span<Render::RenderLight> output,
        const CancellationToken &cancellation) const {
        const auto bounds = Math::TransformAabb(instance.localBounds, instance.localToWorld);
        if (bounds.HasError())
            return Result<Render::ForwardDrawLightSelection>::Failure(bounds.ErrorValue());
        return Render::PrepareDrawForwardLights(std::span{lights_}.first(count_), bounds.Value(), budget_, output, cancellation);
    }

    Result<Render::ForwardDrawLightSelection> EditorViewportForwardLighting::SelectDraw(const Render::RenderSceneView &scene,
                                                                                        const Render::RenderStaticMeshInstance &instance,
                                                                                        const std::span<Render::RenderLight> output,
                                                                                        const CancellationToken &cancellation) const {
        if (!MatchesScene(scene))
            return Result<Render::ForwardDrawLightSelection>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        return SelectInstance(instance, output, cancellation);
    }

    Result<std::optional<Render::DirectionalShadowFrame>> EditorViewportForwardLighting::RemapShadow(
        const std::optional<Render::DirectionalShadowFrame> &shadow, const Render::ForwardDrawLightSelection &selection,
        const Render::RenderSceneView &scene) const {
        using Outcome = Result<std::optional<Render::DirectionalShadowFrame>>;
        if (!MatchesScene(scene) || selection.count > Render::MaximumForwardLights)
            return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        if (!shadow)
            return Outcome::Success(std::nullopt);
        if (shadow->lightIndex >= scene.lights.size())
            return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        const auto identity = scene.lights[shadow->lightIndex].stableIdentity;
        for (std::size_t local = 0; local < selection.count; ++local) {
            const auto source = selection.sourceIndices[local];
            if (source >= count_)
                return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            if (lights_[source].identity.value == identity) {
                auto remapped = *shadow;
                remapped.lightIndex = local;
                return Outcome::Success(remapped);
            }
        }
        return Outcome::Success(std::nullopt);
    }

    EditorViewportLightingDiagnostics EditorViewportForwardLighting::Diagnostics() const noexcept {
        return diagnostics_;
    }
}  // namespace Horo::Editor
