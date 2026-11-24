#pragma once

/** @file AudioCaptureMonitor.h @brief Control-lane admission and bounded PCM reads for local input monitoring. */
#include "Horo/Audio/Internal/AudioCaptureBuffer.h"

namespace Horo::Audio::Backend {
    /** @brief Product-owned verification scoped to the current playback endpoint. */
    enum class AudioMonitorIsolation : std::uint8_t {
        Unverified,
        PrivateListening
    };

    /** @brief Echo cancellation may opt in to the program tap upstream of the microphone monitor. */
    enum class AudioEchoReferenceTap : std::uint8_t {
        Disabled,
        ProgramBeforeMonitor
    };

    /** @brief Explicit one-generation request for a non-amplifying, local-only mixer route. */
    struct AudioCaptureMonitorRoute final {
        AudioCaptureDeviceId input;
        std::uint64_t captureGeneration{};
        AudioDeviceEpoch output;
        AudioBusHandle bus;
        std::uint64_t clockDomain{};
        std::uint64_t maximumAgeNanoseconds{}; /**< Queue age at consumption, not measured end-to-end latency. */
        AudioSample gain{};
        AudioMonitorIsolation isolation{};
        AudioEchoReferenceTap echoReference{};
        bool operator==(const AudioCaptureMonitorRoute &) const noexcept = default;
    };

    /** @brief Discarding an old or future block never publishes it as playable PCM. */
    enum class AudioMonitorRead : std::uint8_t {
        Copied,
        Empty,
        InsufficientSpace,
        Cancelled,
        Expired
    };

    /** @brief Invocation-only interleaved output reference from the admitted program tap. */
    struct AudioEchoReferenceBlock final {
        AudioDeviceEpoch output;
        AudioEchoReferenceTap tap{};
        AudioMonotonicTimestamp start;
        const AudioProcessingFormat &format; /**< Borrowed output layout and sample rate for alignment during this invocation. */
        std::uint32_t frames{};
        std::span<const AudioSample> samples;
    };

    /** @brief Validates exact source, active playback, bus scope, gain, queue age, and private-listening authorization. */
    [[nodiscard]] Result<void> AdmitAudioCaptureMonitor(const AudioCaptureMonitorRoute &route, const AudioCaptureBufferDescriptor &capture,
                                                        const AudioDeviceEpoch &activeOutput);

    /** @brief Reads one admitted monitor block without waiting or keeping a buffer reference.
     * @pre Control/consumer lane. The host revalidates route and playback admission before each read.
     * @return Copy status. On expiry the copied sample prefix is scrubbed and the metadata destination is unchanged.
     */
    [[nodiscard]] AudioMonitorRead ReadAudioMonitorBlock(AudioCaptureBuffer &buffer, const AudioCaptureMonitorRoute &route,
                                                         AudioMonotonicTimestamp now, AudioCapturedBlock &block,
                                                         std::span<AudioSample> output) noexcept;

    /** @brief Checks that a reference is timely, complete and from the opted-in pre-monitor output tap.
     * The output owner supplies the true tap and PCM shape. The consumer owns alignment and any echo processing.
     */
    [[nodiscard]] bool AcceptAudioEchoReference(const AudioCaptureMonitorRoute &route, const AudioEchoReferenceBlock &reference,
                                                const AudioDeviceEpoch &activeOutput, AudioMonotonicTimestamp now) noexcept;
}  // namespace Horo::Audio::Backend
