#include "AudioSourceAnalysis.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace Horo::Audio::AudioImportDetail {
    namespace {
        constexpr float SilenceDb = -200.0F;

        [[nodiscard]] Error MakeImportError(const ErrorCodeDescriptor &descriptor) {
            return MakeError(descriptor);
        }

        [[nodiscard]] float Decibels(const long double amplitude) noexcept {
            return amplitude > 0 ? static_cast<float>(20.0L * std::log10(amplitude)) : SilenceDb;
        }

        [[nodiscard]] Result<AudioLoudnessMetadata> Loudness(const AnalysisAccumulator &analysis, const std::uint32_t sampleRate,
                                                             const std::uint64_t frames) {
            const long double meanSquare = analysis.sampleCount == 0 ? 0 : analysis.squaredSum / analysis.sampleCount;
            double integrated{};
            double shortTerm{};
            if (ebur128_loudness_global(analysis.meter.get(), &integrated) != EBUR128_SUCCESS ||
                ebur128_loudness_shortterm(analysis.meter.get(), &shortTerm) != EBUR128_SUCCESS)
                return Result<AudioLoudnessMetadata>::Failure(MakeImportError(AudioErrors::SourceDecodeFailed));
            double truePeak{};
            for (unsigned channel = 0; channel < analysis.meter->channels; ++channel) {
                double channelPeak{};
                if (ebur128_true_peak(analysis.meter.get(), channel, &channelPeak) != EBUR128_SUCCESS)
                    return Result<AudioLoudnessMetadata>::Failure(MakeImportError(AudioErrors::SourceDecodeFailed));
                truePeak = std::max(truePeak, channelPeak);
            }
            AudioLoudnessMetadata metadata;
            if (std::isfinite(integrated)) {
                metadata.integratedLufs = static_cast<float>(integrated);
                metadata.normalizationGainDb = -23.0F - *metadata.integratedLufs;
            }
            if (frames >= static_cast<std::uint64_t>(sampleRate) * 3 && std::isfinite(shortTerm))
                metadata.shortTermLufs = static_cast<float>(shortTerm);
            metadata.truePeakDbtp = Decibels(truePeak);
            metadata.rmsDbfs = Decibels(std::sqrt(meanSquare));
            return Result<AudioLoudnessMetadata>::Success(metadata);
        }

        [[nodiscard]] std::vector<AudioWaveformLevel> WaveformLevels(std::vector<AudioWaveformPoint> points,
                                                                     const std::uint32_t baseWindowFrames) {
            std::vector<AudioWaveformLevel> levels;
            std::uint64_t windowFrames = baseWindowFrames;
            while (!points.empty()) {
                levels.emplace_back(windowFrames, std::move(points));
                const auto &previous = levels.back().points;
                if (previous.size() == 1)
                    break;
                points.clear();
                points.reserve((previous.size() + 1) / 2);
                for (std::size_t index = 0; index < previous.size(); index += 2) {
                    const auto &first = previous[index];
                    if (index + 1 == previous.size()) {
                        points.push_back(first);
                    } else {
                        const auto &second = previous[index + 1];
                        points.emplace_back(first.firstFrame, first.frameCount + second.frameCount, std::min(first.minimum, second.minimum),
                                            std::max(first.maximum, second.maximum));
                    }
                }
                windowFrames *= 2;
            }
            return levels;
        }
    }  // namespace

    void AnalysisAccumulator::MeterDeleter::operator()(ebur128_state *state) const noexcept {
        ebur128_destroy(&state);
    }

    Result<void> AnalysisAccumulator::Configure(const AudioProcessingFormat &format) {
        using enum AudioSpeakerRole;
        const auto channels = format.layout.orderedChannels.size();
        meter.reset(
            ebur128_init(static_cast<unsigned>(channels), format.sampleRate, EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_TRUE_PEAK));
        if (!meter)
            return Result<void>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
        for (std::size_t index = 0; index < channels; ++index) {
            const auto *role = std::get_if<AudioSpeakerRole>(&format.layout.orderedChannels[index]);
            if (!role)
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
            int channel{};
            switch (*role) {
                case FrontLeft:
                    channel = EBUR128_LEFT;
                    break;
                case FrontRight:
                    channel = EBUR128_RIGHT;
                    break;
                case FrontCenter:
                    channel = EBUR128_CENTER;
                    break;
                case LowFrequency:
                    channel = EBUR128_UNUSED;
                    break;
                case BackLeft:
                case SideLeft:
                    channel = EBUR128_LEFT_SURROUND;
                    break;
                case BackRight:
                case SideRight:
                    channel = EBUR128_RIGHT_SURROUND;
                    break;
                default:
                    return Result<void>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
            }
            if (ebur128_set_channel(meter.get(), static_cast<unsigned>(index), channel) != EBUR128_SUCCESS)
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
        }
        return Result<void>::Success();
    }

    Result<void> AnalysisAccumulator::Consume(const std::span<const AudioSample> samples, const std::uint32_t channels,
                                              const std::uint32_t maximumWindowFrames) {
        const auto frames = static_cast<std::uint32_t>(samples.size() / channels);
        if (std::ranges::any_of(samples, [](const AudioSample sample) {
            return !std::isfinite(sample);
        }))
            return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        if (const auto status = ebur128_add_frames_float(meter.get(), samples.data(), frames); status != EBUR128_SUCCESS)
            return Result<void>::Failure(
                MakeImportError(status == EBUR128_ERROR_NOMEM ? AudioErrors::SourceLimitExceeded : AudioErrors::SourceDecodeFailed));
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const AudioSample sample = samples[static_cast<std::size_t>(frame) * channels + channel];
                const AudioSample magnitude = std::abs(sample);
                peak = std::max(peak, magnitude);
                windowMinimum = std::min(windowMinimum, sample);
                windowMaximum = std::max(windowMaximum, sample);
                squaredSum += static_cast<long double>(sample) * sample;
                ++sampleCount;
            }
            ++windowFrames;
            if (windowFrames == maximumWindowFrames)
                FlushWindow();
        }
        return Result<void>::Success();
    }

    void AnalysisAccumulator::FlushWindow() {
        if (windowFrames == 0)
            return;
        waveform.emplace_back(windowStart, windowFrames, windowMinimum, windowMaximum);
        windowStart += windowFrames;
        windowFrames = 0;
        windowMinimum = std::numeric_limits<AudioSample>::max();
        windowMaximum = std::numeric_limits<AudioSample>::lowest();
    }

    Result<AudioAnalysisMetadata> FinishAnalysis(AnalysisAccumulator &accumulator, const std::uint32_t sampleRate,
                                                 const std::uint64_t frames, const std::uint64_t blockFrames,
                                                 const std::uint32_t waveformWindowFrames, const std::size_t channels) {
        auto loudness = Loudness(accumulator, sampleRate, frames);
        if (loudness.HasError())
            return Result<AudioAnalysisMetadata>::Failure(loudness.ErrorValue());
        auto levels = WaveformLevels(std::move(accumulator.waveform), waveformWindowFrames);
        std::uint64_t waveformBytes{};
        for (const auto &level : levels)
            waveformBytes += level.points.size() * 20U;
        AudioAnalysisMetadata analysis{
            .waveformLevels = std::move(levels),
            .loudness = std::move(loudness).Value(),
            .samplePeak = accumulator.peak,
            .residentPcmBytes = frames * channels * sizeof(AudioSample),
            .decodeBlockBytes = blockFrames * channels * sizeof(AudioSample),
            .waveformBytes = waveformBytes,
        };
        return Result<AudioAnalysisMetadata>::Success(std::move(analysis));
    }
}  // namespace Horo::Audio::AudioImportDetail
