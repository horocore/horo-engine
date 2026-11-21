#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/NativeAudioDriver.h"

#include <algorithm>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Apply the same native-constraint provenance to every changed admitted preference. */
        AudioFormatDeviationReason DeviceDeviation(const bool unchanged) noexcept {
            return unchanged ? AudioFormatDeviationReason::None : AudioFormatDeviationReason::DeviceConstraint;
        }

        /** @brief Admit only exact-rate complete speaker permutations, without implicit conversion or channel loss. */
        bool MatchesNativeSignal(const AudioProcessingFormat &format, const AudioProcessingFormat &native) {
            return format.sampleRate == native.sampleRate && format.layout.kind == native.layout.kind &&
                   format.layout.orderedChannels.size() == native.layout.orderedChannels.size() &&
                   std::is_permutation(format.layout.orderedChannels.begin(), format.layout.orderedChannels.end(),
                                       native.layout.orderedChannels.begin());
        }

        /** @brief Preserve caller preference order while selecting only an explicitly admitted native-compatible tuple. */
        const AudioProcessingFormat *SelectNativeFormat(const AudioDeviceFormatRequest &request, const AudioProcessingFormat &native) {
            if (MatchesNativeSignal(request.preferred, native))
                return &request.preferred;
            const auto found =
                std::find_if(request.allowedAlternatives.begin(), request.allowedAlternatives.end(), [&native](const auto &format) {
                return MatchesNativeSignal(format, native);
            });
            return found == request.allowedAlternatives.end() ? nullptr : &*found;
        }
    }  // namespace

    /** @copydoc NegotiateNativeFloatOutput */
    Result<AudioNegotiatedDeviceFormat> NegotiateNativeFloatOutput(const Open &request, const AudioProcessingFormat &native,
                                                                   const std::uint64_t revision, const std::uint32_t period) {
        if (!ValidateAudioDeviceFormatRequest(request.format) || !ValidateAudioProcessingFormat(native) || revision == 0 ||
            period < request.format.period.minimumFrames || period > request.format.period.maximumFrames)
            return Result<AudioNegotiatedDeviceFormat>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        const auto *effective = SelectNativeFormat(request.format, native);
        if (!effective)
            return Result<AudioNegotiatedDeviceFormat>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        AudioNegotiatedDeviceFormat format{.device = request.plannedEpoch.device,
                                           .discoveryRevision = revision,
                                           .formatRevision = request.plannedEpoch.formatRevision,
                                           .effective = *effective,
                                           .nativeSignal = native,
                                           .nativePcm = {},
                                           .nativeChannelForHoro = {},
                                           .callbackFrames = period,
                                           .nativePeriodFrames = period};
        for (const auto &channel : effective->layout.orderedChannels)
            format.nativeChannelForHoro.push_back(
                static_cast<std::uint8_t>(std::find(native.layout.orderedChannels.begin(), native.layout.orderedChannels.end(), channel) -
                                          native.layout.orderedChannels.begin()));
        format.deviations.sampleRate = DeviceDeviation(effective->sampleRate == request.format.preferred.sampleRate);
        format.deviations.layout = DeviceDeviation(effective->layout == request.format.preferred.layout);
        format.deviations.callbackPeriod = DeviceDeviation(period == request.format.period.preferredFrames);
        return Result<AudioNegotiatedDeviceFormat>::Success(std::move(format));
    }

}  // namespace Horo::Audio::Backend
