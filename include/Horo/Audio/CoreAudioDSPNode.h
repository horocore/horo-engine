#pragma once

/** @file CoreAudioDSPNode.h
 * @brief Deterministic prepared gain, pan and first-order filters for canonical audio blocks.
 */

#include "Horo/Audio/AudioDSPNode.h"

namespace Horo::Audio {
    /** @brief Explicit baseline operation; filters use a matched-exponential one-pole response. */
    enum class CoreAudioDSPKind : std::uint8_t {
        Gain,
        Pan,
        LowPass,
        HighPass
    };

    /**
     * @brief One callback-safe built-in strategy with host-owned, predeclared channel state.
     *
     * Construct on control, then Prepare before processing. Gain admits [0,16]; pan admits
     * [-1,1]; cutoff admits [1,0.45*sampleRate] Hz. The descriptor owns one local parameter ID.
     * Gain and filters preserve all valid layouts. Pan accepts exactly the canonical Mono or
     * Stereo speaker preset, outputs Stereo, uses equal-power mono pan and stereo balance
     * (unity at center); it does not perform multichannel/spatial conversion.
     *
     * Each parameter snapshot is an explicit linear segment: sample n uses value +
     * (target-value)*min(n+1,rampFrames)/rampFrames; zero ramp uses target immediately.
     * The host supplies the next segment's start/remaining duration across block boundaries.
     * Filters retain per-plane low-pass history; high-pass is input minus that low-pass.
     * Internal headroom is preserved. Subnormal outputs/state become positive-zero silence.
     *
     * Prepare borrows state storage until reprepare/destruction; the host keeps it alive.
     * Process must supply that exact storage base and obey the prepared frame limit. Exact
     * corresponding-plane in-place processing is supported; all other input/output overlap
     * is rejected. Reset clears filter history. Bypass freezes history and copies input
     * (mono pan duplicates at unity) or writes silence; it does not consume filter tails.
     * Filters declare a conservative bounded 32-time-constant tail at the minimum cutoff.
     * The graph feeds explicit silence during that interval. Remaining tail is reported;
     * exhausted tails clear history before the next sample. No node owns tail scheduling.
     */
    class CoreAudioDSPNode final : public IAudioDSPNode {
    public:
        /** @brief Builds inert requirements on control. @param kind Operation. @param format Input format.
         * @param maximumFrames Admitted frame bound. @throws std::bad_alloc On descriptor allocation.
         * Invalid kinds/formats/layouts are rejected by Prepare; inspect Descriptor before activation.
         */
        CoreAudioDSPNode(CoreAudioDSPKind kind, AudioProcessingFormat format, std::uint32_t maximumFrames = MaximumAudioCallbackFrames);
        CoreAudioDSPNode(const CoreAudioDSPNode &) = delete;
        CoreAudioDSPNode &operator=(const CoreAudioDSPNode &) = delete;
        /** @brief Returns immutable, complete requirements. @return Stable descriptor. */
        [[nodiscard]] const AudioDSPNodeDescriptor &Descriptor() const noexcept override;
        /** @brief Validates and binds host storage, clearing history. @param context Host-owned memory.
         * @return Success or typed admission failure; a failed reprepare preserves the previous binding.
         */
        [[nodiscard]] Result<void> Prepare(const AudioDSPPrepareContext &context) override;
        /** @brief Processes one explicit parameter segment. @param context Borrowed invocation.
         * @return Bounded result; invalid calls do not modify samples/history. Nonfinite input/output
         * faults clear the output and history, allowing the host to record a callback fault.
         */
        [[nodiscard]] AudioDSPProcessResult Process(const AudioDSPProcessContext &context) noexcept override;
        /** @brief Clears bound history without allocating; safe before preparation. */
        void Reset() noexcept override;

    private:
        CoreAudioDSPKind m_Kind;
        AudioDSPNodeDescriptor m_Descriptor;
        std::span<std::byte> m_State;
        std::uint32_t m_MaximumFrames{};
    };
}  // namespace Horo::Audio
