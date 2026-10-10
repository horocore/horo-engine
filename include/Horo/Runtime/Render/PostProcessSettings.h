#pragma once

/** @file PostProcessSettings.h
 * @brief Owned backend-neutral post-process settings and explicit group overrides.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"

#include <cstdint>
#include <optional>

namespace Horo::Render {
    /** @brief Exposed-luminance bloom settings; threshold is in base-2 stops relative to 0.18. */
    struct BloomSettings {
        float thresholdEv{0.0F};
        float intensity{1.0F};
        float scatter{0.5F};
        std::uint32_t downsampleLevels{5};
        bool operator==(const BloomSettings &) const = default;
    };

    /** @brief Camera/lens settings in world units, f-stops, and millimetres. */
    struct DepthOfFieldSettings {
        float focusDistance{10.0F};
        float apertureFStop{2.8F};
        float focalLengthMm{50.0F};
        std::uint32_t sampleCount{16};
        bool operator==(const DepthOfFieldSettings &) const = default;
    };

    /** @brief Motion-blur settings consuming declared depth, velocity, and history. */
    struct MotionBlurSettings {
        float intensity{1.0F};
        float maxBlurPixels{32.0F};
        std::uint32_t sampleCount{16};
        bool operator==(const MotionBlurSettings &) const = default;
    };

    /** @brief Explicit authored ambient-occlusion algorithm; no implicit algorithm fallback. */
    enum class AmbientOcclusionMode : std::uint8_t {
        Ssao,
        Gtao,
        Hbao
    };

    /** @brief Ambient visibility parameters consumed before scene lighting/composition. */
    struct AmbientOcclusionSettings {
        AmbientOcclusionMode mode{AmbientOcclusionMode::Gtao};
        float radius{1.0F};
        float intensity{1.0F};
        float power{1.0F};
        std::uint32_t sampleCount{16};
        bool operator==(const AmbientOcclusionSettings &) const = default;
    };

    /** @brief Bounded reflection tracing parameters with an explicit history dependency. */
    struct ScreenSpaceReflectionsSettings {
        float intensity{1.0F};
        float maxDistance{100.0F};
        float thickness{0.1F};
        std::uint32_t maxSteps{32};
        bool operator==(const ScreenSpaceReflectionsSettings &) const = default;
    };

    /** @brief Scene-referred vignette parameters. */
    struct VignetteSettings {
        float intensity{0.25F};
        float roundness{1.0F};
        float smoothness{0.5F};
        bool operator==(const VignetteSettings &) const = default;
    };

    /** @brief Scene-referred chromatic displacement in pixels. */
    struct ChromaticAberrationSettings {
        float intensityPixels{1.0F};
        bool operator==(const ChromaticAberrationSettings &) const = default;
    };

    /** @brief Scene-referred grain strength and scale with a stable authored seed. */
    struct FilmGrainSettings {
        float intensity{0.1F};
        float scale{1.0F};
        std::uint32_t seed{1};
        bool operator==(const FilmGrainSettings &) const = default;
    };

    /** @brief Creative look parameters forwarded to the color-pipeline owner, before its output transform. */
    struct ColorGradingSettings {
        float temperatureKelvin{6500.0F};
        float tint{0.0F};
        float saturation{1.0F};
        float contrast{1.0F};
        float gamma{1.0F};
        Math::Vec4 shadows{1.0F, 1.0F, 1.0F, 0.0F};
        Math::Vec4 midtones{1.0F, 1.0F, 1.0F, 0.0F};
        Math::Vec4 highlights{1.0F, 1.0F, 1.0F, 0.0F};
        bool operator==(const ColorGradingSettings &) const = default;
    };

    /** @brief Complete value settings; absent effects are disabled. No exposure or output authority is owned here. */
    struct PostProcessSettings {
        std::optional<BloomSettings> bloom;
        std::optional<DepthOfFieldSettings> depthOfField;
        std::optional<MotionBlurSettings> motionBlur;
        std::optional<AmbientOcclusionSettings> ambientOcclusion;
        std::optional<ScreenSpaceReflectionsSettings> reflections;
        std::optional<VignetteSettings> vignette;
        std::optional<ChromaticAberrationSettings> chromaticAberration;
        std::optional<FilmGrainSettings> filmGrain;
        float exposureCompensationEv{0.0F}; /**< Desired compensation only; the exposure owner publishes its generation. */
        ColorGradingSettings colorGrading;
        bool operator==(const PostProcessSettings &) const = default;
    };

    /** @brief Distinguishes inheritance from an explicit effect disable or settings replacement. */
    enum class PostProcessOverrideMode : std::uint8_t {
        Inherit,
        Disable,
        Replace
    };

    /** @brief An owned override of one typed effect group. */
    template <typename T> struct PostProcessOverride {
        PostProcessOverrideMode mode{PostProcessOverrideMode::Inherit};
        T settings{}; /**< Used only for Replace. */
    };

    /** @brief Volume/profile overrides; numeric values blend while group presence and discrete fields switch at weight 0.5. */
    struct PostProcessSettingsOverrides {
        PostProcessOverride<BloomSettings> bloom;
        PostProcessOverride<DepthOfFieldSettings> depthOfField;
        PostProcessOverride<MotionBlurSettings> motionBlur;
        PostProcessOverride<AmbientOcclusionSettings> ambientOcclusion;
        PostProcessOverride<ScreenSpaceReflectionsSettings> reflections;
        PostProcessOverride<VignetteSettings> vignette;
        PostProcessOverride<ChromaticAberrationSettings> chromaticAberration;
        PostProcessOverride<FilmGrainSettings> filmGrain;
        std::optional<float> exposureCompensationEv;
        std::optional<ColorGradingSettings> colorGrading;
    };

    /** @brief Validates every authored group and finite numeric bound. @param settings Complete value settings.
     * @return Success or a typed invalid-settings failure.
     */
    [[nodiscard]] Result<void> ValidatePostProcessSettings(const PostProcessSettings &settings);
}  // namespace Horo::Render
