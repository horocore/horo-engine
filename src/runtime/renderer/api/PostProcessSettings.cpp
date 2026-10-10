#include "Horo/Runtime/Render/PostProcessSettings.h"

#include "Horo/Runtime/Render/PostProcessErrors.h"
#include "PostProcessSettingsInternal.h"

#include <cmath>

namespace Horo::Render {
    namespace {
        /** @brief Tests inclusive finite authoring bounds without normalizing invalid input. */
        [[nodiscard]] bool Bounded(const float value, const float minimum, const float maximum) noexcept {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        /** @brief Tests a finite strictly positive authored distance/scale. */
        [[nodiscard]] bool Positive(const float value) noexcept {
            return Bounded(value, 0.0001F, 1'000'000.0F);
        }

        /** @brief Tests finite bounded shader work counts. */
        [[nodiscard]] bool Samples(const std::uint32_t value) noexcept {
            return value > 0 && value <= 256;
        }

        /** @brief Validates photographic bloom controls and finite downsample work. */
        [[nodiscard]] bool Valid(const BloomSettings &s) noexcept {
            return Bounded(s.thresholdEv, -32, 32) && Bounded(s.intensity, 0, 64) && Bounded(s.scatter, 0, 1) && s.downsampleLevels > 0 &&
                   s.downsampleLevels <= 16;
        }

        /** @brief Validates physical lens units and bounded bokeh work. */
        [[nodiscard]] bool Valid(const DepthOfFieldSettings &s) noexcept {
            return Positive(s.focusDistance) && Bounded(s.apertureFStop, 0.1F, 128) && Bounded(s.focalLengthMm, 0.1F, 2000) &&
                   Samples(s.sampleCount);
        }

        /** @brief Validates motion strength, displacement and work. */
        [[nodiscard]] bool Valid(const MotionBlurSettings &s) noexcept {
            return Bounded(s.intensity, 0, 64) && Bounded(s.maxBlurPixels, 0, 4096) && Samples(s.sampleCount);
        }

        /** @brief Validates explicit occlusion mode and controls. */
        [[nodiscard]] bool Valid(const AmbientOcclusionSettings &s) noexcept {
            return static_cast<std::uint8_t>(s.mode) <= static_cast<std::uint8_t>(AmbientOcclusionMode::Hbao) && Positive(s.radius) &&
                   Bounded(s.intensity, 0, 64) && Positive(s.power) && Samples(s.sampleCount);
        }

        /** @brief Validates reflection trace controls. */
        [[nodiscard]] bool Valid(const ScreenSpaceReflectionsSettings &s) noexcept {
            return Bounded(s.intensity, 0, 64) && Positive(s.maxDistance) && Positive(s.thickness) && Samples(s.maxSteps);
        }

        /** @brief Validates vignette controls. */
        [[nodiscard]] bool Valid(const VignetteSettings &s) noexcept {
            return Bounded(s.intensity, 0, 1) && Bounded(s.roundness, 0, 1) && Bounded(s.smoothness, 0, 1);
        }

        /** @brief Validates finite chromatic displacement. */
        [[nodiscard]] bool Valid(const ChromaticAberrationSettings &s) noexcept {
            return Bounded(s.intensityPixels, 0, 128);
        }

        /** @brief Validates grain strength and scale; every seed is valid. */
        [[nodiscard]] bool Valid(const FilmGrainSettings &s) noexcept {
            return Bounded(s.intensity, 0, 1) && Positive(s.scale);
        }

        /** @brief Validates scene-referred look parameters without selecting an output transform. */
        [[nodiscard]] bool Valid(const ColorGradingSettings &s) noexcept {
            const auto wheel = [](const Math::Vec4 value) {
                return Bounded(value.x, -64, 64) && Bounded(value.y, -64, 64) && Bounded(value.z, -64, 64) && Bounded(value.w, -64, 64);
            };
            return Bounded(s.temperatureKelvin, 1000, 40000) && Bounded(s.tint, -1, 1) && Bounded(s.saturation, 0, 8) &&
                   Bounded(s.contrast, 0, 8) && Bounded(s.gamma, 0.1F, 8) && wheel(s.shadows) && wheel(s.midtones) && wheel(s.highlights);
        }

        /** @brief Disabled groups have no active parameter contract. */
        template <typename T> [[nodiscard]] bool ValidOptional(const std::optional<T> &s) noexcept {
            return !s.has_value() || Valid(*s);
        }

        /** @brief Only Replace reads a group's settings payload. */
        template <typename T> [[nodiscard]] bool ValidOverride(const PostProcessOverride<T> &s) noexcept {
            using enum PostProcessOverrideMode;
            return s.mode == Inherit || s.mode == Disable || (s.mode == Replace && Valid(s.settings));
        }

        /** @brief Convex interpolation over admitted finite bounds. */
        [[nodiscard]] float Blend(const float a, const float b, const float t) noexcept {
            return std::lerp(a, b, t);
        }

        /** @brief Discrete authoring values use a deterministic midpoint switch. */
        template <typename T> [[nodiscard]] T Pick(const T a, const T b, const float t) noexcept {
            return t < 0.5F ? a : b;
        }

        /** @brief Blends photographic bloom controls. */
        [[nodiscard]] BloomSettings Blend(const BloomSettings &a, const BloomSettings &b, const float t) noexcept {
            return {Blend(a.thresholdEv, b.thresholdEv, t), Blend(a.intensity, b.intensity, t), Blend(a.scatter, b.scatter, t),
                    Pick(a.downsampleLevels, b.downsampleLevels, t)};
        }

        /** @brief Blends physical lens controls, preserving discrete work counts. */
        [[nodiscard]] DepthOfFieldSettings Blend(const DepthOfFieldSettings &a, const DepthOfFieldSettings &b, const float t) noexcept {
            return {Blend(a.focusDistance, b.focusDistance, t), Blend(a.apertureFStop, b.apertureFStop, t),
                    Blend(a.focalLengthMm, b.focalLengthMm, t), Pick(a.sampleCount, b.sampleCount, t)};
        }

        /** @brief Blends motion-blur controls. */
        [[nodiscard]] MotionBlurSettings Blend(const MotionBlurSettings &a, const MotionBlurSettings &b, const float t) noexcept {
            return {Blend(a.intensity, b.intensity, t), Blend(a.maxBlurPixels, b.maxBlurPixels, t), Pick(a.sampleCount, b.sampleCount, t)};
        }

        /** @brief Blends AO controls without interpolating algorithm identities. */
        [[nodiscard]] AmbientOcclusionSettings Blend(const AmbientOcclusionSettings &a, const AmbientOcclusionSettings &b,
                                                     const float t) noexcept {
            return {Pick(a.mode, b.mode, t), Blend(a.radius, b.radius, t), Blend(a.intensity, b.intensity, t), Blend(a.power, b.power, t),
                    Pick(a.sampleCount, b.sampleCount, t)};
        }

        /** @brief Blends reflection controls. */
        [[nodiscard]] ScreenSpaceReflectionsSettings Blend(const ScreenSpaceReflectionsSettings &a, const ScreenSpaceReflectionsSettings &b,
                                                           const float t) noexcept {
            return {Blend(a.intensity, b.intensity, t), Blend(a.maxDistance, b.maxDistance, t), Blend(a.thickness, b.thickness, t),
                    Pick(a.maxSteps, b.maxSteps, t)};
        }

        /** @brief Blends vignette controls. */
        [[nodiscard]] VignetteSettings Blend(const VignetteSettings &a, const VignetteSettings &b, const float t) noexcept {
            return {Blend(a.intensity, b.intensity, t), Blend(a.roundness, b.roundness, t), Blend(a.smoothness, b.smoothness, t)};
        }

        /** @brief Blends chromatic displacement. */
        [[nodiscard]] ChromaticAberrationSettings Blend(const ChromaticAberrationSettings &a, const ChromaticAberrationSettings &b,
                                                        const float t) noexcept {
            return {Blend(a.intensityPixels, b.intensityPixels, t)};
        }

        /** @brief Blends grain controls while retaining a discrete seed. */
        [[nodiscard]] FilmGrainSettings Blend(const FilmGrainSettings &a, const FilmGrainSettings &b, const float t) noexcept {
            return {Blend(a.intensity, b.intensity, t), Blend(a.scale, b.scale, t), Pick(a.seed, b.seed, t)};
        }

        /** @brief Blends look controls and wheels in their authored scene-referred parameter space. */
        [[nodiscard]] ColorGradingSettings Blend(const ColorGradingSettings &a, const ColorGradingSettings &b, const float t) noexcept {
            return {Blend(a.temperatureKelvin, b.temperatureKelvin, t),
                    Blend(a.tint, b.tint, t),
                    Blend(a.saturation, b.saturation, t),
                    Blend(a.contrast, b.contrast, t),
                    Blend(a.gamma, b.gamma, t),
                    Math::Lerp(a.shadows, b.shadows, t),
                    Math::Lerp(a.midtones, b.midtones, t),
                    Math::Lerp(a.highlights, b.highlights, t)};
        }

        /** @brief Applies one admitted optional group without allocating or mutating profile state. */
        template <typename T> void Apply(std::optional<T> &current, const PostProcessOverride<T> &over, const float weight) noexcept {
            using enum PostProcessOverrideMode;
            if (weight == 0 || over.mode == Inherit)
                return;
            if (over.mode == Disable) {
                if (weight >= 0.5F)
                    current.reset();
            } else if (current.has_value()) {
                current = Blend(*current, over.settings, weight);
            } else if (weight >= 0.5F) {
                current = over.settings;
            }
        }

        /** @brief A volume's explicit override takes precedence over its reusable profile. */
        template <typename T>
        [[nodiscard]] PostProcessOverride<T> Resolve(const std::optional<T> &profile, const PostProcessOverride<T> &over) noexcept {
            if (over.mode != PostProcessOverrideMode::Inherit)
                return over;
            if (profile.has_value())
                return {PostProcessOverrideMode::Replace, *profile};
            return {PostProcessOverrideMode::Disable, {}};
        }
    }  // namespace

    /** @copydoc ValidatePostProcessSettings */
    Result<void> ValidatePostProcessSettings(const PostProcessSettings &s) {
        if (!ValidOptional(s.bloom) || !ValidOptional(s.depthOfField) || !ValidOptional(s.motionBlur) ||
            !ValidOptional(s.ambientOcclusion) || !ValidOptional(s.reflections) || !ValidOptional(s.vignette) ||
            !ValidOptional(s.chromaticAberration) || !ValidOptional(s.filmGrain) || !Bounded(s.exposureCompensationEv, -32, 32) ||
            !Valid(s.colorGrading))
            return Result<void>::Failure(MakeError(PostProcessErrors::InvalidSettings));
        return Result<void>::Success();
    }

    namespace Detail {
        /** @copydoc ValidatePostProcessOverrides */
        Result<void> ValidatePostProcessOverrides(const PostProcessSettingsOverrides &s) {
            if (!ValidOverride(s.bloom) || !ValidOverride(s.depthOfField) || !ValidOverride(s.motionBlur) ||
                !ValidOverride(s.ambientOcclusion) || !ValidOverride(s.reflections) || !ValidOverride(s.vignette) ||
                !ValidOverride(s.chromaticAberration) || !ValidOverride(s.filmGrain) ||
                (s.exposureCompensationEv.has_value() && !Bounded(*s.exposureCompensationEv, -32, 32)) ||
                (s.colorGrading.has_value() && !Valid(*s.colorGrading)))
                return Result<void>::Failure(MakeError(PostProcessErrors::InvalidSettings));
            return Result<void>::Success();
        }

        /** @copydoc BlendPostProcessOverrides */
        void BlendPostProcessOverrides(PostProcessSettings &s, const PostProcessSettingsOverrides &o, const float weight) noexcept {
            Apply(s.bloom, o.bloom, weight);
            Apply(s.depthOfField, o.depthOfField, weight);
            Apply(s.motionBlur, o.motionBlur, weight);
            Apply(s.ambientOcclusion, o.ambientOcclusion, weight);
            Apply(s.reflections, o.reflections, weight);
            Apply(s.vignette, o.vignette, weight);
            Apply(s.chromaticAberration, o.chromaticAberration, weight);
            Apply(s.filmGrain, o.filmGrain, weight);
            if (o.exposureCompensationEv.has_value())
                s.exposureCompensationEv = Blend(s.exposureCompensationEv, *o.exposureCompensationEv, weight);
            if (o.colorGrading.has_value())
                s.colorGrading = Blend(s.colorGrading, *o.colorGrading, weight);
        }

        /** @copydoc ResolvePostProcessProfile */
        PostProcessSettingsOverrides ResolvePostProcessProfile(const PostProcessSettings &p,
                                                               const PostProcessSettingsOverrides &o) noexcept {
            return {Resolve(p.bloom, o.bloom),
                    Resolve(p.depthOfField, o.depthOfField),
                    Resolve(p.motionBlur, o.motionBlur),
                    Resolve(p.ambientOcclusion, o.ambientOcclusion),
                    Resolve(p.reflections, o.reflections),
                    Resolve(p.vignette, o.vignette),
                    Resolve(p.chromaticAberration, o.chromaticAberration),
                    Resolve(p.filmGrain, o.filmGrain),
                    o.exposureCompensationEv.value_or(p.exposureCompensationEv),
                    o.colorGrading.value_or(p.colorGrading)};
        }
    }  // namespace Detail
}  // namespace Horo::Render
