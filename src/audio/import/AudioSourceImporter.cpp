#include "Horo/Audio/AudioSourceImporter.h"

#include "Horo/Audio/AudioErrors.h"

#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
// miniaudio consumes the header-only declarations above; this include emits the single decoder implementation.
#include <stb_vorbis.c>
#include <type_traits>
#include <utility>

namespace Horo::Audio {
    namespace AudioImportDetail {
        constexpr std::size_t RiffHeaderBytes = 12;
        constexpr std::size_t ChunkHeaderBytes = 8;
        constexpr std::size_t WaveFormatMinimumBytes = 16;
        constexpr std::size_t OggPageHeaderBytes = 27;
        constexpr float SilenceDb = -200.0F;

        struct ReaderState final {
            const AudioSourceReader &source;
            const AudioSourceImportLimits &limits;
            std::uint64_t cursor{};
            std::uint64_t cumulativeBytes{};
            std::uint32_t operations{};
            std::optional<Error> failure;
        };

        struct SourceProbe final {
            AudioContainerId container;
            AudioCodecId codec;
            std::optional<AudioPcmFormat> pcm;
            std::vector<AudioLoopRegion> loops;
        };

        [[nodiscard]] Error MakeImportError(const ErrorCodeDescriptor &descriptor) {
            return MakeError(descriptor);
        }

        [[nodiscard]] Error DecoderFailure(const ReaderState &reader) {
            return reader.failure ? *reader.failure : MakeImportError(AudioErrors::SourceDecodeFailed);
        }

        [[nodiscard]] bool CheckedAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t &sum) noexcept {
            if (const auto available = std::numeric_limits<std::uint64_t>::max() - left; right <= available) {
                sum = left + right;
                return true;
            }
            return false;
        }

        [[nodiscard]] bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right, std::uint64_t &product) noexcept {
            if (const bool representable = left == 0 || right <= std::numeric_limits<std::uint64_t>::max() / left; representable) {
                product = left * right;
                return true;
            }
            return false;
        }

        template <typename Integer> [[nodiscard]] Integer ReadLittle(const std::span<const std::byte> bytes) noexcept {
            static_assert(std::is_unsigned_v<Integer>);
            Integer value{};
            for (std::size_t index = 0; index < sizeof(Integer); ++index)
                value |= static_cast<Integer>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
            return value;
        }

        [[nodiscard]] bool EqualFour(const std::span<const std::byte> bytes, const char (&text)[5]) noexcept {
            return bytes.size() >= 4 && std::to_integer<unsigned char>(bytes[0]) == static_cast<unsigned char>(text[0]) &&
                   std::to_integer<unsigned char>(bytes[1]) == static_cast<unsigned char>(text[1]) &&
                   std::to_integer<unsigned char>(bytes[2]) == static_cast<unsigned char>(text[2]) &&
                   std::to_integer<unsigned char>(bytes[3]) == static_cast<unsigned char>(text[3]);
        }

        [[nodiscard]] Result<void> ReadExact(ReaderState &state, const std::uint64_t offset, const std::span<std::byte> destination) {
            if (std::uint64_t end{}; !CheckedAdd(offset, destination.size(), end) || end > state.source.size)
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
            if (state.operations == state.limits.maximumReadOperations ||
                destination.size() > state.limits.maximumCumulativeReadBytes - state.cumulativeBytes)
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
            ++state.operations;
            state.cumulativeBytes += destination.size();
            auto read = state.source.readAt(state.source.context, offset, destination);
            if (read.HasError())
                return Result<void>::Failure(read.ErrorValue());
            if (read.Value() != destination.size())
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceReadFailed));
            return Result<void>::Success();
        }

#include "AudioSourceProbeDetail.inl"

        [[nodiscard]] ma_result OnRead(ma_decoder *decoder, void *buffer, const std::size_t requested, std::size_t *bytesRead) noexcept {
            auto &state = *static_cast<ReaderState *>(decoder->pUserData);
            *bytesRead = 0;
            if (state.failure || state.cursor > state.source.size)
                return MA_ERROR;
            const auto remaining = state.source.size - state.cursor;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, requested));
            if (count == 0)
                return MA_AT_END;
            if (state.operations == state.limits.maximumReadOperations ||
                count > state.limits.maximumCumulativeReadBytes - state.cumulativeBytes) {
                state.failure = MakeImportError(AudioErrors::SourceLimitExceeded);
                return MA_ERROR;
            }
            ++state.operations;
            state.cumulativeBytes += count;
            auto result = state.source.readAt(state.source.context, state.cursor, {static_cast<std::byte *>(buffer), count});
            if (result.HasError()) {
                state.failure = result.ErrorValue();
                return MA_ERROR;
            }
            if (result.Value() == 0 || result.Value() > count) {
                state.failure = MakeImportError(AudioErrors::SourceReadFailed);
                return MA_ERROR;
            }
            state.cursor += result.Value();
            *bytesRead = result.Value();
            return MA_SUCCESS;
        }

        [[nodiscard]] ma_result OnSeek(ma_decoder *decoder, const ma_int64 offset, const ma_seek_origin origin) noexcept {
            auto &state = *static_cast<ReaderState *>(decoder->pUserData);
            if (state.failure)
                return MA_ERROR;
            std::uint64_t base{};
            switch (origin) {
                case ma_seek_origin_start:
                    break;
                case ma_seek_origin_current:
                    base = state.cursor;
                    break;
                case ma_seek_origin_end:
                    base = state.source.size;
                    break;
                default:
                    return MA_BAD_SEEK;
            }
            std::uint64_t destination{};
            if (offset < 0) {
                const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
                if (magnitude > base)
                    return MA_BAD_SEEK;
                destination = base - magnitude;
            } else if (!CheckedAdd(base, static_cast<std::uint64_t>(offset), destination) || destination > state.source.size) {
                return MA_BAD_SEEK;
            }
            state.cursor = destination;
            return MA_SUCCESS;
        }

        [[nodiscard]] std::optional<AudioSpeakerRole> SpeakerRole(const ma_channel channel) noexcept {
            using enum AudioSpeakerRole;
            switch (channel) {
                case MA_CHANNEL_MONO:
                case MA_CHANNEL_FRONT_CENTER:
                    return FrontCenter;
                case MA_CHANNEL_FRONT_LEFT:
                    return FrontLeft;
                case MA_CHANNEL_FRONT_RIGHT:
                    return FrontRight;
                case MA_CHANNEL_LFE:
                    return LowFrequency;
                case MA_CHANNEL_BACK_LEFT:
                    return BackLeft;
                case MA_CHANNEL_BACK_RIGHT:
                    return BackRight;
                case MA_CHANNEL_SIDE_LEFT:
                    return SideLeft;
                case MA_CHANNEL_SIDE_RIGHT:
                    return SideRight;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] Result<AudioChannelLayout> MakeLayout(const std::span<const ma_channel> channels) {
            AudioChannelLayout layout;
            layout.orderedChannels.reserve(channels.size());
            for (const auto channel : channels) {
                const auto role = SpeakerRole(channel);
                if (!role)
                    return Result<AudioChannelLayout>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
                layout.orderedChannels.emplace_back(*role);
            }
            if (!ValidateAudioChannelLayout(ViewAudioChannelLayout(layout), MaximumCoreAudioSourceChannels))
                return Result<AudioChannelLayout>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
            return Result<AudioChannelLayout>::Success(std::move(layout));
        }

        class Decoder final {
        public:
            Decoder() = default;
            Decoder(const Decoder &) = delete;
            Decoder &operator=(const Decoder &) = delete;

            ~Decoder() {
                if (initialized_)
                    ma_decoder_uninit(&value_);
            }

            [[nodiscard]] ma_decoder *Get() noexcept {
                return &value_;
            }

            void MarkInitialized() noexcept {
                initialized_ = true;
            }

        private:
            ma_decoder value_{};
            bool initialized_{};
        };

        struct AnalysisAccumulator final {
            std::vector<AudioWaveformPoint> waveform;
            long double squaredSum{};
            std::uint64_t sampleCount{};
            AudioSample peak{};
            AudioSample windowMinimum{std::numeric_limits<AudioSample>::max()};
            AudioSample windowMaximum{std::numeric_limits<AudioSample>::lowest()};
            std::uint32_t windowFrames{};
            std::uint64_t windowStart{};

            void Consume(const std::span<const AudioSample> samples, const std::uint32_t channels,
                         const std::uint32_t maximumWindowFrames) {
                const auto frames = static_cast<std::uint32_t>(samples.size() / channels);
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
            }

            void FlushWindow() {
                if (windowFrames == 0)
                    return;
                waveform.emplace_back(windowStart, windowFrames, windowMinimum, windowMaximum);
                windowStart += windowFrames;
                windowFrames = 0;
                windowMinimum = std::numeric_limits<AudioSample>::max();
                windowMaximum = std::numeric_limits<AudioSample>::lowest();
            }
        };

        [[nodiscard]] float Decibels(const long double amplitude) noexcept {
            return amplitude > 0 ? static_cast<float>(20.0L * std::log10(amplitude)) : SilenceDb;
        }

        [[nodiscard]] AudioLoudnessMetadata Loudness(const AnalysisAccumulator &analysis) noexcept {
            const long double meanSquare = analysis.sampleCount == 0 ? 0 : analysis.squaredSum / analysis.sampleCount;
            const float integrated = meanSquare > 0 ? static_cast<float>(-0.691L + 10.0L * std::log10(meanSquare)) : SilenceDb;
            return {.integratedLufs = integrated,
                    .shortTermLufs = std::nullopt,
                    .truePeakDbtp = Decibels(analysis.peak),
                    .rmsDbfs = Decibels(std::sqrt(meanSquare)),
                    .normalizationGainDb = -23.0F - integrated};
        }

        [[nodiscard]] bool ValidSourceLimits(const AudioSourceImportLimits &limits) noexcept {
            return limits.maximumSourceBytes > 0 && limits.maximumCumulativeReadBytes >= limits.maximumSourceBytes &&
                   limits.maximumReadOperations > 0;
        }

        [[nodiscard]] bool ValidDecodeLimits(const AudioSourceImportLimits &limits) noexcept {
            return limits.maximumDecodedFrames > 0 && limits.maximumDecodedBytes > 0 && limits.decodeBlockFrames > 0 &&
                   limits.maximumChannels > 0 && limits.maximumChannels <= MaximumCoreAudioSourceChannels;
        }

        [[nodiscard]] bool ValidAnalysisLimits(const AudioSourceImportLimits &limits) noexcept {
            return limits.waveformWindowFrames > 0 && limits.maximumWaveformPoints > 0 &&
                   limits.maximumLoopRegions <= MaximumAudioAssetLoopRegions;
        }

        [[nodiscard]] bool ValidLimits(const AudioSourceImportLimits &limits) noexcept {
            return ValidSourceLimits(limits) && ValidDecodeLimits(limits) && ValidAnalysisLimits(limits);
        }

        struct DecodePlan final {
            AudioProcessingFormat format;
            std::uint64_t declaredFrames{};
            std::uint64_t blockFrames{};
            std::uint64_t waveformPoints{};
            bool hasDeclaredFrames{};
        };

        [[nodiscard]] bool SupportedDecodedFormat(const ma_format sampleFormat, const ma_uint32 channels, const ma_uint32 sampleRate,
                                                  const std::uint32_t maximumChannels) noexcept {
            return sampleFormat == ma_format_f32 && channels > 0 && channels <= maximumChannels && sampleRate >= MinimumAudioSampleRate &&
                   sampleRate <= MaximumAudioSampleRate;
        }

        [[nodiscard]] Result<AudioProcessingFormat> ReadProcessingFormat(Decoder &decoder, const AudioSourceImportLimits &limits) {
            ma_format sampleFormat{};
            ma_uint32 channels{};
            ma_uint32 sampleRate{};
            std::array<ma_channel, MaximumCoreAudioSourceChannels> channelMap{};
            if (const auto result =
                    ma_decoder_get_data_format(decoder.Get(), &sampleFormat, &channels, &sampleRate, channelMap.data(), channelMap.size());
                result != MA_SUCCESS || !SupportedDecodedFormat(sampleFormat, channels, sampleRate, limits.maximumChannels))
                return Result<AudioProcessingFormat>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
            auto layout = MakeLayout(std::span{channelMap}.first(channels));
            if (layout.HasError())
                return Result<AudioProcessingFormat>::Failure(layout.ErrorValue());
            AudioProcessingFormat format{sampleRate, std::move(layout).Value()};
            return ValidateAudioProcessingFormat(format, limits.maximumChannels)
                       ? Result<AudioProcessingFormat>::Success(std::move(format))
                       : Result<AudioProcessingFormat>::Failure(MakeImportError(AudioErrors::SourceUnsupported));
        }

        [[nodiscard]] bool DecodedExtentWithinLimits(const std::uint64_t frames, const std::uint64_t channels,
                                                     const AudioSourceImportLimits &limits) noexcept {
            std::uint64_t samples{};
            std::uint64_t bytes{};
            return frames <= limits.maximumDecodedFrames && CheckedMultiply(frames, channels, samples) &&
                   CheckedMultiply(samples, sizeof(AudioSample), bytes) && bytes <= limits.maximumDecodedBytes;
        }

        [[nodiscard]] Result<std::pair<std::uint64_t, bool>> ReadDeclaredFrames(Decoder &decoder, const SourceProbe &probe) {
            ma_uint64 frames{};
            const bool available = ma_decoder_get_length_in_pcm_frames(decoder.Get(), &frames) == MA_SUCCESS && frames > 0;
            if (!available && probe.container != AudioContainerIds::Ogg)
                return Result<std::pair<std::uint64_t, bool>>::Failure(MakeImportError(AudioErrors::SourceInvalid));
            return Result<std::pair<std::uint64_t, bool>>::Success({frames, available});
        }

        [[nodiscard]] Result<DecodePlan> MakeDecodePlan(Decoder &decoder, const SourceProbe &probe, const AudioSourceImportLimits &limits) {
            auto format = ReadProcessingFormat(decoder, limits);
            if (format.HasError())
                return Result<DecodePlan>::Failure(format.ErrorValue());
            auto declared = ReadDeclaredFrames(decoder, probe);
            if (declared.HasError())
                return Result<DecodePlan>::Failure(declared.ErrorValue());
            const auto [frameCount, hasFrames] = declared.Value();
            const auto channels = format.Value().layout.orderedChannels.size();
            if (hasFrames && !DecodedExtentWithinLimits(frameCount, channels, limits))
                return Result<DecodePlan>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
            const std::uint64_t waveformPoints = hasFrames ? 1 + (frameCount - 1) / limits.waveformWindowFrames : 0;
            if (hasFrames && waveformPoints > limits.maximumWaveformPoints)
                return Result<DecodePlan>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
            const auto bytesPerFrame = static_cast<std::uint64_t>(channels) * sizeof(AudioSample);
            const auto blockFrames = std::min<std::uint64_t>(
                {limits.decodeBlockFrames, limits.maximumDecodedFrames, limits.maximumDecodedBytes / bytesPerFrame});
            if (blockFrames == 0)
                return Result<DecodePlan>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
            return Result<DecodePlan>::Success({std::move(format).Value(), frameCount, blockFrames, waveformPoints, hasFrames});
        }

        struct DecodedAnalysis final {
            std::uint64_t frames{};
            AnalysisAccumulator analysis;
        };

        [[nodiscard]] Result<std::uint64_t> ReadDecodedBlock(Decoder &decoder, const ReaderState &reader,
                                                             const std::span<AudioSample> samples, const std::uint64_t requestedFrames) {
            ma_uint64 framesRead{};
            const auto readResult = ma_decoder_read_pcm_frames(decoder.Get(), samples.data(), requestedFrames, &framesRead);
            if (framesRead == 0 && (readResult == MA_SUCCESS || readResult == MA_AT_END))
                return Result<std::uint64_t>::Success(0);
            if ((readResult != MA_SUCCESS && readResult != MA_AT_END) || framesRead == 0 || framesRead > requestedFrames)
                return Result<std::uint64_t>::Failure(DecoderFailure(reader));
            return Result<std::uint64_t>::Success(framesRead);
        }

        [[nodiscard]] Result<std::uint64_t> AdvanceDecodedFrames(const std::uint64_t decodedFrames, const std::uint64_t framesRead,
                                                                 const std::uint64_t bytesPerFrame, const AudioSourceImportLimits &limits) {
            std::uint64_t nextFrame{};
            if (std::uint64_t decodedBytes{};
                !CheckedAdd(decodedFrames, framesRead, nextFrame) || nextFrame > limits.maximumDecodedFrames ||
                !CheckedMultiply(nextFrame, bytesPerFrame, decodedBytes) || decodedBytes > limits.maximumDecodedBytes)
                return Result<std::uint64_t>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
            return Result<std::uint64_t>::Success(nextFrame);
        }

        [[nodiscard]] bool CompleteDecode(const std::uint64_t decodedFrames, const DecodePlan &plan, const AnalysisAccumulator &analysis,
                                          const AudioSourceImportLimits &limits) noexcept {
            const bool frameCountMatches = !plan.hasDeclaredFrames || decodedFrames == plan.declaredFrames;
            const bool waveformCountMatches = !plan.hasDeclaredFrames || analysis.waveform.size() == plan.waveformPoints;
            return decodedFrames > 0 && frameCountMatches && analysis.waveform.size() <= limits.maximumWaveformPoints &&
                   waveformCountMatches;
        }

        [[nodiscard]] Result<void> ConsumeDecodedBlock(AnalysisAccumulator &analysis, const std::span<const AudioSample> block,
                                                       const std::uint32_t channels, const AudioSourceImportLimits &limits) {
            analysis.Consume(block, channels, limits.waveformWindowFrames);
            return analysis.waveform.size() <= limits.maximumWaveformPoints
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
        }

        [[nodiscard]] bool ShouldDecodeMore(const DecodePlan &plan, const std::uint64_t decodedFrames) noexcept {
            return !plan.hasDeclaredFrames || decodedFrames < plan.declaredFrames;
        }

        [[nodiscard]] Result<DecodedAnalysis> DecodeAllFrames(Decoder &decoder, const ReaderState &reader, const DecodePlan &plan,
                                                              const AudioDecodedFrameSink &output, const AudioSourceImportLimits &limits) {
            const auto channels = static_cast<std::uint64_t>(plan.format.layout.orderedChannels.size());
            const auto bytesPerFrame = channels * sizeof(AudioSample);
            std::vector<AudioSample> samples(static_cast<std::size_t>(plan.blockFrames * channels));
            AnalysisAccumulator analysis;
            if (plan.hasDeclaredFrames)
                analysis.waveform.reserve(static_cast<std::size_t>(plan.waveformPoints));
            std::uint64_t decodedFrames{};
            while (ShouldDecodeMore(plan, decodedFrames)) {
                const auto requested =
                    plan.hasDeclaredFrames ? std::min(plan.blockFrames, plan.declaredFrames - decodedFrames) : plan.blockFrames;
                auto read = ReadDecodedBlock(decoder, reader, samples, requested);
                if (read.HasError())
                    return Result<DecodedAnalysis>::Failure(read.ErrorValue());
                const auto framesRead = read.Value();
                if (framesRead == 0)
                    break;
                auto nextFrame = AdvanceDecodedFrames(decodedFrames, framesRead, bytesPerFrame, limits);
                if (nextFrame.HasError())
                    return Result<DecodedAnalysis>::Failure(nextFrame.ErrorValue());
                const auto block = std::span<const AudioSample>{samples}.first(static_cast<std::size_t>(framesRead * channels));
                if (const auto written = output.write(output.context, decodedFrames, plan.format, block); written.HasError())
                    return Result<DecodedAnalysis>::Failure(written.ErrorValue());
                if (const auto consumed = ConsumeDecodedBlock(analysis, block, static_cast<std::uint32_t>(channels), limits);
                    consumed.HasError())
                    return Result<DecodedAnalysis>::Failure(consumed.ErrorValue());
                decodedFrames = nextFrame.Value();
            }
            analysis.FlushWindow();
            if (!CompleteDecode(decodedFrames, plan, analysis, limits))
                return Result<DecodedAnalysis>::Failure(MakeImportError(AudioErrors::SourceDecodeFailed));
            return Result<DecodedAnalysis>::Success({decodedFrames, std::move(analysis)});
        }

        [[nodiscard]] Result<SourceProbe> ProbeSource(ReaderState &reader) {
            std::array<std::byte, 4> magic{};
            if (const auto read = ReadExact(reader, 0, magic); read.HasError())
                return Result<SourceProbe>::Failure(read.ErrorValue());
            return EqualFour(magic, "RIFF") ? ProbeWave(reader) : ProbeOggVorbis(reader);
        }

        [[nodiscard]] Result<void> InitializeDecoder(Decoder &decoder, ReaderState &reader) {
            if (auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
                ma_decoder_init(&OnRead, &OnSeek, &reader, &config, decoder.Get()) != MA_SUCCESS)
                return Result<void>::Failure(DecoderFailure(reader));
            decoder.MarkInitialized();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const AudioSourceReader &source, const AudioDecodedFrameSink &output,
                                                   const AudioSourceImportLimits &limits) {
            if (!source.readAt || !output.write || source.size < RiffHeaderBytes || !ValidLimits(limits))
                return Result<void>::Failure(MakeImportError(AudioErrors::SourceInvalid));
            return source.size <= limits.maximumSourceBytes ? Result<void>::Success()
                                                            : Result<void>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
        }
    }  // namespace AudioImportDetail

    /** @copydoc ImportCoreAudioSource */
    Result<AudioSourceImportCandidate> ImportCoreAudioSource(const AudioSourceReader &source, const AudioDecodedFrameSink &output,
                                                             const AudioSourceImportLimits &limits) {
        using namespace AudioImportDetail;
        if (const auto request = ValidateRequest(source, output, limits); request.HasError())
            return Result<AudioSourceImportCandidate>::Failure(request.ErrorValue());

        ReaderState reader{source, limits};
        auto probe = ProbeSource(reader);
        if (probe.HasError())
            return Result<AudioSourceImportCandidate>::Failure(probe.ErrorValue());

        reader.cursor = 0;
        Decoder decoder;
        if (const auto initialized = InitializeDecoder(decoder, reader); initialized.HasError())
            return Result<AudioSourceImportCandidate>::Failure(initialized.ErrorValue());
        auto sourceProbe = std::move(probe).Value();
        auto plan = MakeDecodePlan(decoder, sourceProbe, limits);
        if (plan.HasError())
            return Result<AudioSourceImportCandidate>::Failure(plan.ErrorValue());
        auto planValue = std::move(plan).Value();
        auto decoded = DecodeAllFrames(decoder, reader, planValue, output, limits);
        if (decoded.HasError())
            return Result<AudioSourceImportCandidate>::Failure(decoded.ErrorValue());
        auto decodedValue = std::move(decoded).Value();
        if (std::ranges::any_of(sourceProbe.loops, [&decodedValue](const AudioLoopRegion &loop) {
            return loop.endFrame > decodedValue.frames;
        }))
            return Result<AudioSourceImportCandidate>::Failure(MakeImportError(AudioErrors::SourceInvalid));
        std::uint64_t durationNumerator{};
        if (!CheckedMultiply(decodedValue.frames, 1'000'000'000ULL, durationNumerator))
            return Result<AudioSourceImportCandidate>::Failure(MakeImportError(AudioErrors::SourceLimitExceeded));
        const auto duration = durationNumerator / planValue.format.sampleRate;
        return Result<AudioSourceImportCandidate>::Success({.container = sourceProbe.container,
                                                            .codec = sourceProbe.codec,
                                                            .sourcePcm = sourceProbe.pcm,
                                                            .decodedFormat = std::move(planValue.format),
                                                            .frameCount = decodedValue.frames,
                                                            .durationNanoseconds = duration,
                                                            .loops = std::move(sourceProbe.loops),
                                                            .waveform = std::move(decodedValue.analysis.waveform),
                                                            .loudness = Loudness(decodedValue.analysis),
                                                            .samplePeak = decodedValue.analysis.peak,
                                                            .decoderIdentity = "miniaudio/0.11.25"});
    }
}  // namespace Horo::Audio
