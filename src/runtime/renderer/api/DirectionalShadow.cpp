#include "DirectionalShadowInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Render {
    namespace DirectionalShadowErrors {
        namespace {
            const ErrorDomainId Domain{"render.directional_shadow"};
        }

        const ErrorCodeDescriptor InvalidProfile{Domain, ErrorCode{"render.directional_shadow.invalid_profile"}, ErrorSeverity::Error,
                                                 "Directional shadow profile exceeds its finite budget.",
                                                 "Use one to four guarded tiles and finite supported filter and split settings."};
        const ErrorCodeDescriptor InvalidScene{Domain, ErrorCode{"render.directional_shadow.invalid_scene"}, ErrorSeverity::Error,
                                               "Directional shadow input scene or camera is invalid.",
                                               "Submit validated camera, lights and caster transforms."};
        const ErrorCodeDescriptor InvalidCoordinates{Domain, ErrorCode{"render.directional_shadow.invalid_coordinates"},
                                                     ErrorSeverity::Error, "Directional shadow coordinates cannot be represented.",
                                                     "Rebase the scene or reduce the admitted shadow distance."};
        const ErrorCodeDescriptor InvalidCache{Domain, ErrorCode{"render.directional_shadow.invalid_cache"}, ErrorSeverity::Error,
                                               "Directional shadow cache identity cannot advance.",
                                               "Provide a live resource incarnation and valid completed frame."};

    }  // namespace DirectionalShadowErrors

    namespace {
        Result<std::optional<DirectionalShadowFrame>> BuildSelected(const RenderSceneView &scene, float aspect,
                                                                    Math::ClipDepthRange depthRange,
                                                                    const DirectionalShadowProfile &profile, std::size_t lightIndex) {
            using Outcome = Result<std::optional<DirectionalShadowFrame>>;
            const auto basis = DirectionalShadowDetail::CameraBasis(scene.camera);
            if (basis.HasError())
                return Outcome::Failure(basis.ErrorValue());
            const auto caster = DirectionalShadowDetail::CasterDepth(scene, scene.lights[lightIndex].direction);
            if (caster.HasError())
                return Outcome::Failure(caster.ErrorValue());
            const auto atlas = ComputeDirectionalShadowAtlasExtent(profile);
            DirectionalShadowFrame frame{.cascadeCount = profile.cascadeCount,
                                         .lightIndex = lightIndex,
                                         .atlasWidth = atlas.Value().width,
                                         .atlasHeight = atlas.Value().height,
                                         .cameraOrigin = scene.camera.position,
                                         .profile = profile};
            for (std::uint32_t index = 0; index < profile.cascadeCount; ++index) {
                auto cascade = DirectionalShadowDetail::FitCascade(scene, basis.Value(), scene.lights[lightIndex].direction, caster.Value(),
                                                                   aspect, depthRange, profile, index, atlas.Value());
                if (cascade.HasError())
                    return Outcome::Failure(cascade.ErrorValue());
                frame.cascades[index] = std::move(cascade).Value();
            }
            frame.fingerprint = DirectionalShadowDetail::Fingerprint(scene, aspect, depthRange, frame);
            return Outcome::Success(frame);
        }

        bool Range(float value, float minimum, float maximum) noexcept {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        bool SceneParameters(const RenderSceneView &scene, float aspect, Math::ClipDepthRange depthRange) noexcept {
            return scene.IsValid() && Range(aspect, 0.001F, 1000.0F) &&
                   (depthRange == Math::ClipDepthRange::NegativeOneToOne || depthRange == Math::ClipDepthRange::ZeroToOne);
        }

        bool SplitSettings(const DirectionalShadowProfile &profile) noexcept {
            return (profile.splitMode == DirectionalShadowSplitMode::Mixed ||
                    profile.splitMode == DirectionalShadowSplitMode::Logarithmic) &&
                   Range(profile.maximumDistance, 0.01F, 100000.0F) && Range(profile.splitLambda, 0.0F, 1.0F) &&
                   Range(profile.blendFraction, 0.0F, 0.5F);
        }

        bool BiasSettings(const DirectionalShadowProfile &profile) noexcept {
            return Range(profile.depthBias, 0.0F, 0.1F) && Range(profile.slopeBias, 0.0F, 0.1F) && Range(profile.normalBias, 0.0F, 10.0F);
        }

        bool ValidRect(const DirectionalShadowViewport &rect, const DirectionalShadowFrame &frame) noexcept {
            return rect.width > 0 && rect.height > 0 && rect.x <= frame.atlasWidth && rect.y <= frame.atlasHeight &&
                   rect.width <= frame.atlasWidth - rect.x && rect.height <= frame.atlasHeight - rect.y;
        }

        bool ValidCascade(const DirectionalShadowCascade &cascade, const DirectionalShadowFrame &frame) noexcept {
            return ValidRect(cascade.viewport, frame) && Math::IsFinite(cascade.viewProjection) &&
                   Math::IsFinite(cascade.cameraRelativeViewProjection) && Range(cascade.splitNear, 0.0F, cascade.splitFar) &&
                   Range(cascade.blendStart, cascade.splitNear, cascade.splitFar) &&
                   Range(cascade.worldUnitsPerTexel, std::numeric_limits<float>::min(), 100000.0F);
        }

        Result<std::optional<DirectionalShadowFrame>> InvalidFrame(const ErrorCodeDescriptor &error) {
            return Result<std::optional<DirectionalShadowFrame>>::Failure(MakeError(error));
        }
    }  // namespace

    bool DirectionalShadowProfile::IsValid() const noexcept {
        const bool tile = tileResolution >= 64 && tileResolution <= 2048 && (tileResolution & (tileResolution - 1)) == 0;
        return cascadeCount >= 1 && cascadeCount <= MaximumDirectionalShadowCascades && tile &&
               filterRadius <= DirectionalShadowGuardPixels && SplitSettings(*this) && BiasSettings(*this);
    }

    DirectionalShadowProfile MakeDirectionalShadowProfile(DirectionalShadowQuality quality) noexcept {
        DirectionalShadowProfile profile;
        switch (quality) {
            case DirectionalShadowQuality::Off:
                profile.enabled = false;
                break;
            case DirectionalShadowQuality::Low:
                profile.cascadeCount = 1;
                profile.tileResolution = 1024;
                profile.filterRadius = 0;
                break;
            case DirectionalShadowQuality::Medium:
                profile.cascadeCount = 2;
                profile.tileResolution = 1024;
                break;
            case DirectionalShadowQuality::High:
                break;
            case DirectionalShadowQuality::Ultra:
                profile.tileResolution = 2048;
                profile.filterRadius = 2;
                break;
            default:
                profile.cascadeCount = 0;  // Unknown enum values cannot silently select a valid budget.
                break;
        }
        return profile;
    }

    Result<DirectionalShadowAtlasExtent> ComputeDirectionalShadowAtlasExtent(const DirectionalShadowProfile &profile) {
        if (!profile.IsValid())
            return Result<DirectionalShadowAtlasExtent>::Failure(MakeError(DirectionalShadowErrors::InvalidProfile));
        if (!profile.enabled)
            return Result<DirectionalShadowAtlasExtent>::Success({});
        const auto columns = std::min(profile.cascadeCount, 2U);
        return Result<DirectionalShadowAtlasExtent>::Success(
            {columns * profile.tileResolution, ((profile.cascadeCount + 1U) / 2U) * profile.tileResolution});
    }

    bool DirectionalShadowFrame::IsValid(const RenderSceneView &scene) const noexcept {
        if (!profile.IsValid() || !profile.enabled || cascadeCount != profile.cascadeCount || fingerprint == 0)
            return false;
        if (lightIndex >= scene.lights.size() || scene.lights[lightIndex].kind != RenderLightKind::Directional)
            return false;
        if (atlasWidth == 0 || atlasHeight == 0 || !Math::IsFinite(cameraOrigin))
            return false;
        return std::ranges::all_of(std::span(cascades).first(cascadeCount), [&](const auto &cascade) {
            return ValidCascade(cascade, *this);
        });
    }

    Result<std::optional<DirectionalShadowFrame>> BuildDirectionalShadowFrame(const RenderSceneView &scene, float aspect,
                                                                              Math::ClipDepthRange depthRange,
                                                                              const DirectionalShadowProfile &profile) {
        using Outcome = Result<std::optional<DirectionalShadowFrame>>;
        if (!profile.IsValid())
            return InvalidFrame(DirectionalShadowErrors::InvalidProfile);
        if (!SceneParameters(scene, aspect, depthRange))
            return InvalidFrame(DirectionalShadowErrors::InvalidScene);
        if (!profile.enabled || scene.instances.empty())
            return Outcome::Success(std::nullopt);
        const auto light = std::ranges::find_if(scene.lights, [](const auto &value) {
            return value.kind == RenderLightKind::Directional && value.intensity > Math::DefaultEpsilon;
        });
        if (light == scene.lights.end())
            return Outcome::Success(std::nullopt);
        if (profile.maximumDistance <= scene.camera.projection.nearPlane)
            return InvalidFrame(DirectionalShadowErrors::InvalidProfile);
        return BuildSelected(scene, aspect, depthRange, profile, static_cast<std::size_t>(light - scene.lights.begin()));
    }

    bool DirectionalShadowRenderRequired(const DirectionalShadowFrame &frame, std::uint64_t resourceIncarnation,
                                         const DirectionalShadowCacheState &cache) noexcept {
        return !frame.cacheable || !cache.valid || cache.fingerprint != frame.fingerprint ||
               cache.resourceIncarnation != resourceIncarnation;
    }

    Result<DirectionalShadowCacheState> CommitDirectionalShadowRender(const DirectionalShadowFrame &frame,
                                                                      std::uint64_t resourceIncarnation,
                                                                      const DirectionalShadowCacheState &previous) {
        using Outcome = Result<DirectionalShadowCacheState>;
        if (frame.fingerprint == 0 || resourceIncarnation == 0 || frame.cascadeCount == 0)
            return Outcome::Failure(MakeError(DirectionalShadowErrors::InvalidCache));
        if (!DirectionalShadowRenderRequired(frame, resourceIncarnation, previous))
            return Outcome::Success(previous);
        if (previous.generation == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(DirectionalShadowErrors::InvalidCache));
        return Outcome::Success({frame.fingerprint, resourceIncarnation, previous.generation + 1, frame.cacheable});
    }
}  // namespace Horo::Render
