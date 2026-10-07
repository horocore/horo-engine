#pragma once

/** @file CoreStereoSpatialRenderer.h
 * @brief Backend-neutral streaming stereo distance, directivity, panning and bounded Doppler processing.
 */
#include "Horo/Audio/AudioResampler.h"
#include "Horo/Audio/AudioSpatialModel.h"

#include <array>

namespace Horo::Audio {
    /** @brief Curves normalized to unity at minimum distance and zero at maximum distance. */
    enum class AudioDistanceCurve : std::uint8_t {
        Linear,
        Inverse,
        InverseSquare
    };

    /** @brief Explicit per-source stereo policy; distances use metres and cone angles are full radians. */
    struct AudioStereoSpatialSettings final {
        AudioDistanceCurve curve{AudioDistanceCurve::Linear};
        float minimumDistance{1.0F};
        float maximumDistance{40.0F};
        float innerConeRadians{2.0F * Math::Pi};
        float outerConeRadians{2.0F * Math::Pi};
        float outerConeGain{1.0F};         /**< [0,1]; linear angular transition between cone half-angles. */
        float spread{};                    /**< [0,1]; reduces directional pan toward center, including stereo sub-source positions. */
        float width{1.0F};                 /**< [0,1]; stereo sub-source pan separation; zero folds stereo to center. */
        float speedOfSound{343.0F};        /**< Finite positive metres/second. Radial velocities clamp to 90 percent of this. */
        float dopplerScale{1.0F};          /**< Finite [0,4] radial-velocity scale. */
        std::uint32_t smoothingFrames{64}; /**< [0,16384] output-sample gain/pan/pitch ramp on each accepted update. */
    };

    /** @brief Immutable control-computed matrix and clip pitch; useful for deterministic reference-scene inspection. */
    struct AudioStereoSpatialTarget final {
        std::array<float, 4> matrix{}; /**< Row-major stereo output by mono/stereo input. */
        double pitch{1.0};             /**< Authored pitch times Doppler, clamped to [0.125,8]. */
        float attenuation{1.0F};       /**< Distance times cone gain, excluding authored voice gain. */
        float pan{};                   /**< Listener-local right projection in [-1,1], before width/spread. */
    };

    /**
     * @brief Evaluate copied scene motion on control, without scene queries or backend selection.
     * @param source Immutable extracted source; gain must be finite in [0,16], pitch positive and at most 8.
     * @param listener Selected listener, required for 3D; ignored for 2D. Weight is applied by the owning listener mix.
     * @param settings Source policy.
     * @param channels Exactly one canonical Mono plane or two ordered Stereo planes.
     * @return Prepared target or ResamplerInvalid; no state or activation side effects.
     * @details Right is local +X, cone forward is local -Z. Coincident sources center with unity cone gain.
     * First/discontinuous source or listener motion suppresses Doppler, even with explicit nonzero velocity.
     * 2D preserves mono equal-power centering or stereo identity and ignores spatial settings/motion.
     */
    [[nodiscard]] Result<AudioStereoSpatialTarget> PrepareAudioStereoSpatialTarget(const AudioSpatialSource &source,
                                                                                   const AudioSpatialListener *listener,
                                                                                   const AudioStereoSpatialSettings &settings,
                                                                                   std::uint32_t channels);

    /**
     * @brief Exclusive-owner prepared streaming PCM processor; one instance belongs to one source/listener pair.
     * @details Create/target preparation/destruction run off callback. Process consumes copied targets and borrowed PCM,
     * without allocation, locks, scene access or backend selection. Callers retain the unconsumed input and end marker,
     * drain until Complete, and route stereo output through their ordinary bus. Canonical input is Mono or ordered
     * Stereo; no layout conversion is implied. This explicit baseline uses unfiltered Linear resampling for continuous
     * Doppler; it makes no anti-aliasing guarantee. Source-to-mix rate conversion and pitch occur exactly once here.
     * The host owns voice lifecycle, loops, listener selection, publication and provider resolution. This processor
     * neither registers a provider nor claims HRTF/ambisonic/native capability. No concurrent operations are permitted.
     */
    class CoreStereoSpatialRenderer final {
    public:
        /** @brief Prepare bounded Linear conversion off callback.
         * @param descriptor Explicit ClipToMix/Linear rates, mono/stereo count, unity pitch/speed and frame bound.
         * @param maximumCoefficientBytes Caller-reserved coefficient-bank byte ceiling.
         * @return Complete owner or typed resampler admission/preparation failure. @throws std::bad_alloc On allocation.
         */
        [[nodiscard]] static Result<CoreStereoSpatialRenderer> Create(const AudioResamplerDescriptor &descriptor,
                                                                      std::uint64_t maximumCoefficientBytes);
        CoreStereoSpatialRenderer(CoreStereoSpatialRenderer &&) noexcept = default;
        CoreStereoSpatialRenderer &operator=(CoreStereoSpatialRenderer &&) noexcept = default;
        CoreStereoSpatialRenderer(const CoreStereoSpatialRenderer &) = delete;
        CoreStereoSpatialRenderer &operator=(const CoreStereoSpatialRenderer &) = delete;
        /**
         * @brief Prepare and apply one copied scene update at an exclusive control/quiescent boundary.
         * @param source Source snapshot.
         * @param listener Explicit selected listener or null for 2D.
         * @param settings Spatial policy.
         * @return Success or typed rejection; failure leaves phase, target and ramps unchanged.
         * @details New identities reset stream history and initialize gains immediately. Changed teleport revisions
         * or discontinuous motion snap pitch to authored pitch and retain gain smoothing. Ordinary updates ramp
         * gains/pitch by produced output samples; splitting processing blocks cannot restart a ramp.
         */
        [[nodiscard]] Result<void> Update(const AudioSpatialSource &source, const AudioSpatialListener *listener,
                                          const AudioStereoSpatialSettings &settings);
        /** @brief Read the last admitted target on the exclusive owner. @return Copied target. */
        [[nodiscard]] AudioStereoSpatialTarget Target() const noexcept;
        /** @brief Stream PCM into stereo with exact consumed/produced counts and bounded tail.
         * @param input Borrowed mono/stereo input; same buffer requirements as AudioResampler::Process.
         * @param output Two distinct aligned output planes bounded by the prepared maximumOutputFrames.
         * @return Progress; rejected calls do not modify output or state, and unwritten output remains untouched.
         * @pre Update must succeed before processing. Input and output must not overlap this owner or each other.
         */
        [[nodiscard]] AudioResamplerProgress Process(AudioResamplerInput input, AudioResamplerOutput output) noexcept;
        /** @brief Clear stream history and finish ramps at the retained target; caller owns discontinuity presentation. */
        void Reset() noexcept;

    private:
        explicit CoreStereoSpatialRenderer(AudioResampler converter, const AudioResamplerDescriptor &descriptor) noexcept;
        /** @brief Advance one gain ramp and write one already resampled stereo frame.
         * @param output Admitted borrowed destination.
         * @param progress Exact frame cursor and sanitization counter, advanced by one frame.
         */
        void EmitStereo(AudioResamplerOutput output, AudioResamplerProgress &progress) noexcept;
        AudioResampler converter_;
        AudioResamplerDescriptor descriptor_;
        AudioStereoSpatialTarget target_;
        std::array<float, 4> matrix_{};
        std::uint32_t remaining_{};
        AudioSpatialIdentity source_;
        AudioSpatialIdentity listener_;
        std::uint64_t sourceRevision_{};
        std::uint64_t listenerRevision_{};
        bool initialized_{};

        struct alignas(64) SampleCell final {
            std::array<float, 16> samples{};
        };

        std::array<SampleCell, 2> inputCells_{};
        std::array<SampleCell, 2> outputCells_{};
    };
}  // namespace Horo::Audio
