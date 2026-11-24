#include "Horo/Audio/Internal/AudioCaptureMonitor.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Security/SecureMemory.h"

#include <algorithm>
#include <cmath>

namespace Horo::Audio::Backend {
    namespace {
        constexpr std::uint64_t MaximumQueueAgeNs = 250'000'000;

        /** @brief Matches the exact admitted capture identity and rejects empty scope values. */
        bool BoundCapture(const AudioCaptureMonitorRoute &route, const AudioCaptureBufferDescriptor &capture) noexcept {
            return route.input.IsValid() && route.captureGeneration != 0 && route.clockDomain != 0 && route.input == capture.device &&
                   route.captureGeneration == capture.generation && route.clockDomain == capture.clockDomain;
        }

        /** @brief Requires independently refreshed output evidence in the same clock domain. */
        bool IsTimely(const AudioCaptureMonitorRoute &route, const AudioMonotonicTimestamp start,
                      const AudioMonotonicTimestamp now) noexcept {
            return start.clockDomain == route.clockDomain && now.clockDomain == route.clockDomain && start.nanoseconds <= now.nanoseconds &&
                   now.nanoseconds - start.nanoseconds <= route.maximumAgeNanoseconds;
        }

        /** @brief Checks only the configured policy; platform isolation verification belongs to the host. */
        bool ValidPolicy(const AudioCaptureMonitorRoute &route) noexcept {
            return route.isolation == AudioMonitorIsolation::PrivateListening && route.maximumAgeNanoseconds > 0 &&
                   route.maximumAgeNanoseconds <= MaximumQueueAgeNs && std::isfinite(route.gain) && route.gain >= 0.0F &&
                   route.gain <= 1.0F &&
                   (route.echoReference == AudioEchoReferenceTap::Disabled ||
                    route.echoReference == AudioEchoReferenceTap::ProgramBeforeMonitor);
        }

        /** @brief Bounds the playable output, including finite source overload and invalid native sample values. */
        AudioSample MonitorSample(const AudioSample sample, const AudioSample gain) noexcept {
            return std::isfinite(sample) ? std::clamp(sample * gain, -1.0F, 1.0F) : 0.0F;
        }

        /** @brief Rejects partial or oversized reference blocks before any echo processor can read them. */
        bool CompleteReference(const AudioEchoReferenceBlock &reference) noexcept {
            const auto channels = reference.format.layout.orderedChannels.size();
            return ValidateAudioProcessingFormat(reference.format) && reference.frames > 0 &&
                   reference.frames <= MaximumAudioCallbackFrames && reference.samples.size() == reference.frames * channels;
        }
    }  // namespace

    /** @copydoc AdmitAudioCaptureMonitor */
    Result<void> AdmitAudioCaptureMonitor(const AudioCaptureMonitorRoute &route, const AudioCaptureBufferDescriptor &capture,
                                          const AudioDeviceEpoch &activeOutput) {
        if (!BoundCapture(route, capture))
            return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (!MatchesAudioDeviceEpoch(activeOutput, route.output))
            return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
        if (!route.bus.IsValid() || route.bus.owner != activeOutput.device.owner)
            return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (!ValidPolicy(route))
            return Result<void>::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        return Result<void>::Success();
    }

    /** @copydoc ReadAudioMonitorBlock */
    AudioMonitorRead ReadAudioMonitorBlock(AudioCaptureBuffer &buffer, const AudioCaptureMonitorRoute &route,
                                           const AudioMonotonicTimestamp now, AudioCapturedBlock &block,
                                           const std::span<AudioSample> output) noexcept {
        const auto *descriptor = buffer.Descriptor();
        if (!descriptor || !BoundCapture(route, *descriptor) || !ValidPolicy(route))
            return AudioMonitorRead::Cancelled;
        AudioCapturedBlock candidate;
        switch (buffer.TryRead(candidate, output)) {
            case AudioCaptureRead::Empty:
                return AudioMonitorRead::Empty;
            case AudioCaptureRead::InsufficientSpace:
                return AudioMonitorRead::InsufficientSpace;
            case AudioCaptureRead::Cancelled:
                return AudioMonitorRead::Cancelled;
            case AudioCaptureRead::Copied:
                break;
        }
        const auto samples = output.first(candidate.frames * descriptor->format.layout.orderedChannels.size());
        if (!IsTimely(route, candidate.start, now)) {
            Security::SecureZero(std::as_writable_bytes(samples));
            return AudioMonitorRead::Expired;
        }
        for (auto &sample : samples)
            sample = MonitorSample(sample, route.gain);
        block = candidate;
        return AudioMonitorRead::Copied;
    }

    /** @copydoc AcceptAudioEchoReference */
    bool AcceptAudioEchoReference(const AudioCaptureMonitorRoute &route, const AudioEchoReferenceBlock &reference,
                                  const AudioDeviceEpoch &activeOutput, const AudioMonotonicTimestamp now) noexcept {
        return ValidPolicy(route) && route.clockDomain != 0 && route.echoReference == AudioEchoReferenceTap::ProgramBeforeMonitor &&
               reference.tap == route.echoReference && MatchesAudioDeviceEpoch(activeOutput, route.output) &&
               reference.output == route.output && IsTimely(route, reference.start, now) && CompleteReference(reference);
    }
}  // namespace Horo::Audio::Backend
