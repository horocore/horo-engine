#include "Horo/Audio/Internal/AudioOutputPolicy.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Validate one independently clocked binding before comparing the two owners. */
        bool ValidBinding(const OutputBinding &output) noexcept {
            return (output.use == OutputUse::Game || output.use == OutputUse::EditorPreview) &&
                   MatchesAudioDeviceEpoch(output.epoch, output.epoch);
        }

        /** @brief Distinguish unsupported synchronization from a malformed future enum value. */
        Result<void> AdmitCompositionMode(const OutputComposition composition) {
            switch (composition) {
                case OutputComposition::Independent:
                    return Result<void>::Success();
                case OutputComposition::Mirrored:
                case OutputComposition::Aggregate:
                    return Result<void>::Failure(MakeError(AudioErrors::OperationUnsupported));
            }
            return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        }
    }  // namespace

    /** @copydoc ResolveBufferPolicy */
    Result<AudioDevicePeriodRequest> ResolveBufferPolicy(const BufferPolicy &policy) {
        if (policy.sampleRate < MinimumAudioSampleRate || policy.sampleRate > MaximumAudioSampleRate || policy.minimumFrames == 0 ||
            policy.minimumFrames > policy.maximumFrames || policy.maximumFrames > MaximumAudioCallbackFrames)
            return Result<AudioDevicePeriodRequest>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        std::uint32_t milliseconds{};
        switch (policy.profile) {
            case BufferProfile::LowLatency:
                milliseconds = 5;
                break;
            case BufferProfile::Stable:
                milliseconds = 20;
                break;
            case BufferProfile::PowerSaving:
                milliseconds = 40;
                break;
            default:
                return Result<AudioDevicePeriodRequest>::Failure(MakeError(AudioErrors::IdentityInvalid));
        }
        const auto preferred = static_cast<std::uint32_t>((static_cast<std::uint64_t>(policy.sampleRate) * milliseconds + 999) / 1'000);
        return Result<AudioDevicePeriodRequest>::Success(
            {policy.minimumFrames, std::clamp(preferred, policy.minimumFrames, policy.maximumFrames), policy.maximumFrames});
    }

    /** @copydoc EstimateSoftwareLatency */
    Result<AudioDurationObservation> EstimateSoftwareLatency(const SoftwareLatencyFrames &frames, const std::uint32_t sampleRate,
                                                             const AudioMonotonicTimestamp capturedAt) {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (capturedAt.clockDomain == 0 || sampleRate < MinimumAudioSampleRate || sampleRate > MaximumAudioSampleRate ||
            frames.dsp > maximum - frames.resampler || frames.dsp + frames.resampler > maximum - frames.decodeQueue)
            return Result<AudioDurationObservation>::Failure(MakeError(AudioErrors::IdentityInvalid));
        const auto total = frames.dsp + frames.resampler + frames.decodeQueue;
        const auto seconds = total / sampleRate;
        const auto remainder = total % sampleRate;
        const auto fractional = (remainder * 1'000'000'000 + sampleRate - 1) / sampleRate;
        if (seconds > (maximum - fractional) / 1'000'000'000)
            return Result<AudioDurationObservation>::Failure(MakeError(AudioErrors::IdentityInvalid));
        return Result<AudioDurationObservation>::Success({.quality = AudioObservationQuality::Estimated,
                                                          .source = AudioObservationSource::Adapter,
                                                          .nanoseconds = seconds * 1'000'000'000 + fractional,
                                                          .observedAt = capturedAt});
    }

    /** @copydoc AdmitOutputComposition */
    Result<void> AdmitOutputComposition(const std::span<const OutputBinding> outputs, const OutputComposition composition) {
        if (const auto mode = AdmitCompositionMode(composition); mode.HasError())
            return mode;
        if (outputs.empty() || outputs.size() > 2)
            return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        for (const auto &output : outputs) {
            if (!ValidBinding(output))
                return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        }
        if (outputs.size() == 2 && (outputs[0].use == outputs[1].use || outputs[0].epoch.device.owner == outputs[1].epoch.device.owner))
            return Result<void>::Failure(MakeError(AudioErrors::IdentityInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PrepareOutputDiagnostics */
    Result<OutputDiagnostics> PrepareOutputDiagnostics(const Open &request, const AudioDeviceSnapshot &devices, const Opened &actual,
                                                       const SoftwareLatencyFrames &software) {
        if (actual.access != request.access || actual.format.device != request.plannedEpoch.device ||
            devices.owner != request.plannedEpoch.device.owner || actual.format.formatRevision != request.plannedEpoch.formatRevision ||
            ValidateAudioDeviceNegotiation(request.format, devices, actual.format).status != AudioDeviceNegotiationStatus::Accepted ||
            !ValidateAudioDeviceTimingReport(actual.timing, request.plannedEpoch, devices.backend))
            return Result<OutputDiagnostics>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        auto estimate = EstimateSoftwareLatency(software, actual.format.effective.sampleRate, actual.timing.capturedAt);
        if (estimate.HasError())
            return Result<OutputDiagnostics>::Failure(estimate.ErrorValue());
        return Result<OutputDiagnostics>::Success({devices.backend, request, actual, software, estimate.Value()});
    }

    /** @copydoc UpdateOutputTiming */
    bool UpdateOutputTiming(OutputDiagnostics &snapshot, const AudioDeviceTimingReport &timing) noexcept {
        if (!ValidateAudioDeviceTimingReport(timing, snapshot.requested.plannedEpoch, snapshot.backend) ||
            timing.capturedAt.clockDomain != snapshot.actual.timing.capturedAt.clockDomain ||
            timing.capturedAt.nanoseconds < snapshot.actual.timing.capturedAt.nanoseconds)
            return false;
        snapshot.actual.timing = timing;
        return true;
    }
}  // namespace Horo::Audio::Backend
