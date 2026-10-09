#pragma once

/** @file AudioResampler.h
 * @brief Bounded, single-owner planar streaming sample-rate conversion.
 */

#include "Horo/Audio/AudioResamplerPlan.h"

#include <memory>
#include <span>

namespace Horo::Audio {
    /** @brief Explicit preparation-time execution selection; unsupported SIMD never falls back silently. */
    enum class AudioResamplerExecution : std::uint8_t {
        Scalar,
        Simd
    };
    /** @brief Allocation-free processing outcome; malformed calls do not consume or write samples. */
    enum class AudioResamplerStatus : std::uint8_t {
        InputNeeded,
        OutputFull,
        Complete,
        InvalidBuffer,
        InvalidState
    };

    /** @brief Borrowed ordered input planes. Unconsumed frames remain owned by the caller. */
    struct AudioResamplerInput {
        std::span<const std::span<const float>> planes;
        std::uint32_t frames{};
        bool endOfStream{}; /**< Marks the end only when all frames in this call have been consumed. */
    };

    /** @brief Borrowed writable output planes, with one shared capacity. */
    struct AudioResamplerOutput {
        std::span<const std::span<float>> planes;
        std::uint32_t capacity{};
    };

    /** @brief Exact progress and bounded fault facts, without allocating a general diagnostic object. */
    struct AudioResamplerProgress {
        AudioResamplerStatus status{AudioResamplerStatus::InvalidState};
        std::uint32_t consumed{};
        std::uint32_t produced{};
        std::uint32_t sanitizedSamples{}; /**< Non-finite input or overflowing/non-finite output replaced with silence. */
        double sourceAdvance{}; /**< Source-frame time advanced by emitted samples, including the actual pitch ramp; excludes look-ahead. */
    };

    /** @brief Prepared single-thread-owned DSP state, independent of any device or runtime lifecycle. */
    class AudioResampler final {
    public:
        /**
         * @brief Allocate coefficients and the plan-reserved history off the callback thread.
         * @param plan Admitted immutable conversion metadata.
         * @param maximumCoefficientBytes Separate coefficient-bank reservation.
         * @param execution Explicit scalar reference or supported native SIMD implementation.
         * @return Prepared state or a typed preparation error; allocation failure may throw std::bad_alloc.
         */
        [[nodiscard]] static Result<AudioResampler> Create(const AudioResamplerPlan &plan, std::uint64_t maximumCoefficientBytes,
                                                           AudioResamplerExecution execution = AudioResamplerExecution::Scalar);
        /** @brief Query this build's baseline SIMD implementation off the callback. @return Whether explicit SIMD preparation is supported.
         */
        [[nodiscard]] static bool SupportsSimd() noexcept;
        /** @brief Release prepared storage only after the single processing owner is quiescent. */
        ~AudioResampler();
        /** @brief Transfer prepared state; the source becomes inert. @param other Quiescent source. */
        AudioResampler(AudioResampler &&other) noexcept;
        /** @brief Replace quiescent state off the callback. @param other Quiescent source. @return This object. */
        AudioResampler &operator=(AudioResampler &&other) noexcept;
        AudioResampler(const AudioResampler &) = delete;
        AudioResampler &operator=(const AudioResampler &) = delete;

        /**
         * @brief Process bounded planar frames without allocating, locking or retaining caller spans.
         * @param input Ordered planes with at least frames elements each; nonempty planes start at 64-byte alignment.
         * @param output Distinct writable planes with at least capacity elements; nonempty planes start at 64-byte alignment.
         * @return Consumed/produced counts, next action and sanitized-sample count. Unwritten output is untouched.
         * @pre Caller owns valid memory and one unchanged semantic layout in the plan's channel order.
         * Input/output and output/output sample ranges must not overlap. Input planes may alias each other.
         * Offered input frames are bounded by maximumOutputFrames * 64 + taps; output capacity by maximumOutputFrames.
         * Re-submit unconsumed input with its end marker. After the marker is consumed, submit empty input until Complete.
         * Drain supplies at most taps zero input frames; it includes the finite filter tail. Empty streams emit no frames.
         * No concurrent Process, Reset, move or destruction is permitted. Destroy/reassign only off the callback after quiescence.
         */
        [[nodiscard]] AudioResamplerProgress Process(AudioResamplerInput input, AudioResamplerOutput output) noexcept;
        /**
         * @brief Change explicit low-cost Linear clip pitch without allocating or discarding fractional history.
         * @param pitch Finite source-speed multiplier in [0.125,8], with rate-adjusted step in [1/64,64].
         * @param rampFrames Linear output-sample ramp, at most 16384; zero applies immediately.
         * @return True on admission; false preserves all state. Sinc/device plans and moved owners reject.
         * @details This unfiltered mode makes no anti-aliasing guarantee. Each produced sample advances
         * the ramp; input starvation and zero-capacity calls freeze it. Reset restores the prepared pitch.
         */
        [[nodiscard]] bool SetLinearPitch(double pitch, std::uint32_t rampFrames = 0) noexcept;
        /** @brief Clear history, phase and end state without allocation; moved-from objects remain inert. */
        void Reset() noexcept;
        /** @brief Read the immutable admitted plan on the exclusive owner. @return Plan or null for a moved owner. */
        [[nodiscard]] const AudioResamplerPlan *Plan() const noexcept;
        /** @brief Check whether no input/end marker has been consumed since preparation/reset. @return Fresh processing state. */
        [[nodiscard]] bool IsFresh() const noexcept;

    private:
        struct State;
        /** @brief Own one completely prepared processing state. */
        explicit AudioResampler(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
