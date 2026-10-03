#pragma once

/** @file AudioVoicePlayback.h
 * @brief Prepared resident PCM playback, canonical voice lifecycle and bounded discontinuities.
 */

#include "Horo/Audio/AudioResampler.h"
#include "Horo/Audio/AudioVoiceControls.h"
#include "Horo/Audio/AudioVoiceStateMachine.h"

namespace Horo::Audio {
    /** @brief Complete prepared voice policy; source and scratch storage have an explicit byte ceiling. */
    struct AudioVoicePlaybackConfig final {
        AudioResamplerPlan plan;
        AudioVoiceLoop loop;
        std::uint32_t rampFrames{64};        /**< Positive bounded output-frame ramp, at most 16384. */
        std::uint64_t maximumStorageBytes{}; /**< Copied PCM plus owner/scratch storage, excluding the resampler reservation. */
        std::uint64_t maximumCoefficientBytes{};
    };

    /** @brief Fixed callback evidence; errors reference static descriptors, never allocated diagnostics. */
    struct AudioVoiceRenderResult final {
        const ErrorCodeDescriptor *error{}; /**< Null on success; translate on control only. */
        std::uint32_t produced{};           /**< Frames containing PCM or a discontinuity ramp; all other requested frames are silence. */
        std::uint32_t sanitizedSamples{};
        bool terminal{}; /**< True only on this call's first terminal transition. */
    };

    /**
     * @brief Sole processing owner of one resident voice, backed by the canonical registry and resampler.
     * @details Create copies PCM and admits a Ready voice off-callback. The registry is borrowed and
     * must outlive this object. Transfer both owners together at a quiescent boundary; Apply, Render,
     * Cursor and SwapPitch require exclusive processing ownership. Creation, moves, destruction and
     * registry Release/BeginShutdown require detached control ownership. No operation is concurrent.
     * Destruction cancels a live voice and releases its slot on control; explicit terminal evidence
     * must be reconciled before destruction. External registry cancellation/shutdown is observed by Render.
     * Resident playback does not decode streams, select devices, route buses or own a sample clock.
     */
    class AudioVoicePlayback final {
    public:
        /**
         * @brief Prepare owned PCM, scratch and DSP before admitting a canonical Ready voice.
         * @param registry Retained exact runtime's canonical voice registry.
         * @param source Borrowed planar PCM, copied before return; frame count may be zero and no end marker is required.
         * @param config Clip-to-mix plan, loop, ramps and explicit storage reservations.
         * @return Owned playback or typed validation, budget, allocation or handle failure, without a leaked slot.
         */
        [[nodiscard]] static Result<AudioVoicePlayback> Create(AudioVoiceStateMachine &registry, AudioResamplerInput source,
                                                               const AudioVoicePlaybackConfig &config);
        /** @brief Cancel/release the canonical slot and destroy storage off-callback after detachment. */
        ~AudioVoicePlayback();
        AudioVoicePlayback(const AudioVoicePlayback &) = delete;
        AudioVoicePlayback &operator=(const AudioVoicePlayback &) = delete;
        /** @brief Transfer quiescent ownership. @param other Source. */
        AudioVoicePlayback(AudioVoicePlayback &&other) noexcept;
        /** @brief Retire old detached ownership and transfer another. @param other Source. @return This owner. */
        AudioVoicePlayback &operator=(AudioVoicePlayback &&other) noexcept;
        /** @brief Read the admitted identity. @return Exact handle or empty for a moved owner. */
        [[nodiscard]] AudioVoiceHandle Voice() const noexcept;
        /** @brief Read processing-owned rendered position. @return Cursor, or zero for a moved owner. */
        [[nodiscard]] AudioVoiceCursor Cursor() const noexcept;
        /**
         * @brief Apply one control without allocation, freeing or blocking, including rejection.
         * @param request Exact handle and typed control; Start requires Ready, Pause Playing, Resume Paused.
         * Seek/loop/rate require Ready, Playing or Paused. Stop accepts Ready, Playing or Paused and
         * overrides an in-flight discontinuity. Cancel accepts any nonterminal voice. Other overlapping
         * controls fail until the ramp completes. Terminal voices never restart.
         * @return Null on success or a static stable descriptor; failure changes neither cursor nor DSP/lifecycle.
         */
        [[nodiscard]] const ErrorCodeDescriptor *Apply(const AudioVoiceControlRequest &request) noexcept;
        /**
         * @brief Exchange a prepared pitch converter at a command boundary without reclaiming either bank.
         * @param voice Exact admitted handle.
         * @param replacement Fresh off-callback prepared clip-to-mix converter with the same rates/channels/quality/block bound.
         * On success it receives the old converter for off-callback retirement and must not be reused as a fresh candidate.
         * @return Null or a stable error descriptor. Resets fractional phase/history, holding the integer cursor,
         * with fade-out/in while playing. Ready/Paused reset immediately. Rejection leaves both owners unchanged.
         */
        [[nodiscard]] const ErrorCodeDescriptor *SwapPitch(AudioVoiceHandle voice, AudioResampler &replacement) noexcept;
        /**
         * @brief Render bounded output, overwriting the requested range with PCM, ramps or positive-zero silence.
         * @param output Distinct 64-byte-aligned planes in the plan's retained semantic channel order.
         * @return Progress, sanitization and terminal-once evidence; malformed buffers remain untouched.
         * @details Start/resume use a linear zero-to-one ramp. Pause/stop/seek/pitch/loop changes hold the last
         * output and ramp it to zero without advancing the source, then commit the reset/hold/terminal action.
         * Seek and loop changes discard filter history/tails and reset fraction; pause preserves them.
         * Natural EOF drains the resampler's bounded tail; looping feeds wrapped PCM without resetting history.
         * Zero-capacity calls do not advance ramps or playback. No caller span is retained.
         */
        [[nodiscard]] AudioVoiceRenderResult Render(AudioResamplerOutput output) noexcept;

    private:
        struct State;
        /** @brief Adopt complete prepared storage. */
        explicit AudioVoicePlayback(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
