#pragma once

/** @file AudioOutputPolicy.h
 * @brief Build-tree-only buffer and independent output admission for host composition.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

namespace Horo::Audio::Backend {
    /** @brief Relative buffering intent; frame bounds are explicit and never hardware guarantees. */
    enum class BufferProfile : std::uint8_t {
        LowLatency,
        Stable,
        PowerSaving
    };

    /** @brief Preserved requested policy, expressed at the canonical processing rate. */
    struct BufferPolicy final {
        BufferProfile profile{BufferProfile::Stable};
        std::uint32_t sampleRate{48'000};
        std::uint32_t minimumFrames{64};
        std::uint32_t maximumFrames{2'048};
    };

    /**
     * @brief Resolve a profile preference within explicit frame limits without querying hardware.
     * @param policy Immutable host intent; accepts canonical sample rates and 1..16384 frame bounds.
     * @return Requested period bounds or a typed input failure; negotiation still determines actual values.
     */
    [[nodiscard]] Result<AudioDevicePeriodRequest> ResolveBufferPolicy(const BufferPolicy &policy);

    /** @brief Disjoint software stages, all in the same canonical sample-rate units. */
    struct SoftwareLatencyFrames final {
        std::uint64_t dsp{};
        std::uint64_t resampler{};
        std::uint64_t decodeQueue{}; /**< Queue depth, not decoder CPU execution time. */
    };

    /**
     * @brief Estimate only disjoint software latency; hardware and backend queues remain separately reported.
     * @param frames Non-overlapping stage contributions supplied by their owning plans.
     * @param sampleRate Valid canonical rate.
     * @param capturedAt Non-zero monotonic clock domain identifying the calculation.
     * @return Adapter estimate rounded up to nanoseconds, or overflow/input failure; never a hardware guarantee.
     */
    [[nodiscard]] Result<AudioDurationObservation> EstimateSoftwareLatency(const SoftwareLatencyFrames &frames, std::uint32_t sampleRate,
                                                                           AudioMonotonicTimestamp capturedAt);

    /** @brief Complete control snapshot; requested policy, native facts and software estimate never overwrite one another. */
    struct OutputDiagnostics final {
        AudioBackendKind backend{AudioBackendKind::NullAudio};
        Open requested;
        Opened actual;
        SoftwareLatencyFrames software;
        AudioDurationObservation softwareLatency;
    };

    /**
     * @brief Build a fresh diagnostics generation after a candidate is admitted, discarding all old-epoch measurements.
     * @param request Preserved immutable requested intent.
     * @param devices Complete selected-backend discovery snapshot.
     * @param actual Candidate format and timing for exactly the planned epoch.
     * @param software Disjoint software stages of the newly prepared mixer/stream plan.
     * @return Complete replacement snapshot or typed admission failure, leaving old publication caller-owned.
     */
    [[nodiscard]] Result<OutputDiagnostics> PrepareOutputDiagnostics(const Open &request, const AudioDeviceSnapshot &devices,
                                                                     const Opened &actual, const SoftwareLatencyFrames &software);
    /**
     * @brief Update only a matching epoch's coherent nondecreasing observations; never reinterpret policy as a measurement.
     * @param snapshot Current admitted control-owned diagnostics generation.
     * @param timing Newly reported facts in the same monotonic domain.
     * @return True after replacement, false with no mutation for stale/malformed evidence.
     */
    [[nodiscard]] bool UpdateOutputTiming(OutputDiagnostics &snapshot, const AudioDeviceTimingReport &timing) noexcept;

    /** @brief One final mixer/device per independently clocked runtime. */
    enum class OutputUse : std::uint8_t {
        Game,
        EditorPreview
    };
    /** @brief Mirroring/aggregation require drift correction that this baseline does not implement. */
    enum class OutputComposition : std::uint8_t {
        Independent,
        Mirrored,
        Aggregate
    };

    /** @brief Immutable routing identity; no common hardware clock is implied by equal clock-domain numbers. */
    struct OutputBinding final {
        OutputUse use{OutputUse::Game};
        AudioDeviceEpoch epoch;
    };

    /**
     * @brief Admit at most one game and one preview runtime, with no implicit duplication or fallback.
     * @param outputs Complete requested composition; one or two valid independently owned epochs.
     * @param composition Independent output, or an explicitly unsupported synchronized mode.
     * @return Success or typed invalid/unsupported result. Each admitted runtime owns its mixer, buffers and clock.
     */
    [[nodiscard]] Result<void> AdmitOutputComposition(std::span<const OutputBinding> outputs, OutputComposition composition);
}  // namespace Horo::Audio::Backend
