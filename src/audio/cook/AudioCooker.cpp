#include "Horo/Audio/AudioCooker.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace Horo::Audio {
    namespace {
        constexpr std::uint32_t MaximumChunkCount = 65'536;

        void Append32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void Append64(std::vector<std::uint8_t> &bytes, const std::uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void AppendDigest(std::vector<std::uint8_t> &bytes, const Sha256Digest &digest) {
            bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
        }

        void AppendFloat(std::vector<std::uint8_t> &bytes, const float value) {
            Append32(bytes, std::bit_cast<std::uint32_t>(value));
        }

        void AppendOptionalFloat(std::vector<std::uint8_t> &bytes, const std::optional<float> value) {
            bytes.push_back(value.has_value() ? 1 : 0);
            if (value.has_value())
                AppendFloat(bytes, *value);
        }

        std::vector<std::uint8_t> EncodeAnalysis(const AudioAnalysisMetadata &analysis) {
            std::vector<std::uint8_t> bytes;
            Append32(bytes, 1);  // Analysis format version, independent of the outer cook schema.
            Append64(bytes, analysis.residentPcmBytes);
            Append64(bytes, analysis.decodeBlockBytes);
            Append64(bytes, analysis.waveformBytes);
            AppendFloat(bytes, analysis.samplePeak);
            AppendOptionalFloat(bytes, analysis.loudness.integratedLufs);
            AppendOptionalFloat(bytes, analysis.loudness.shortTermLufs);
            AppendOptionalFloat(bytes, analysis.loudness.truePeakDbtp);
            AppendOptionalFloat(bytes, analysis.loudness.rmsDbfs);
            AppendOptionalFloat(bytes, analysis.loudness.normalizationGainDb);
            Append32(bytes, static_cast<std::uint32_t>(analysis.waveformLevels.size()));
            for (const auto &level : analysis.waveformLevels) {
                Append64(bytes, level.windowFrames);
                Append32(bytes, static_cast<std::uint32_t>(level.points.size()));
                for (const auto &point : level.points) {
                    Append64(bytes, point.firstFrame);
                    Append32(bytes, point.frameCount);
                    AppendFloat(bytes, point.minimum);
                    AppendFloat(bytes, point.maximum);
                }
            }
            return bytes;
        }

        void AppendLayout(std::vector<std::uint8_t> &bytes, const AudioChannelLayout &layout) {
            bytes.push_back(static_cast<std::uint8_t>(layout.kind));
            bytes.push_back(static_cast<std::uint8_t>(layout.orderedChannels.size()));
            bytes.push_back(layout.ambisonic.has_value() ? layout.ambisonic->order : 0);
            for (const auto &channel : layout.orderedChannels) {
                bytes.push_back(static_cast<std::uint8_t>(channel.index()));
                std::visit([&bytes]<typename Role>(const Role role) {
                    if constexpr (std::is_same_v<Role, AudioSpeakerRole>)
                        bytes.push_back(static_cast<std::uint8_t>(role));
                    else if constexpr (std::is_same_v<Role, AudioDiscreteChannel>)
                        bytes.push_back(role.index);
                    else
                        bytes.push_back(role.acn);
                }, channel);
            }
        }

        struct MemoryReader final {
            std::span<const std::byte> bytes;

            // AudioSourceReader's C-style callback borrows this exact stack object only during synchronous import.
            static Result<std::size_t> Read(void *context, const std::uint64_t offset, const std::span<std::byte> destination) {
                const auto &self = *static_cast<MemoryReader *>(context);
                if (offset > self.bytes.size())
                    return Result<std::size_t>::Failure(MakeError(AudioErrors::SourceReadFailed));
                const auto count = std::min(destination.size(), self.bytes.size() - static_cast<std::size_t>(offset));
                std::ranges::copy(self.bytes.subspan(static_cast<std::size_t>(offset), count), destination.begin());
                return Result<std::size_t>::Success(count);
            }
        };

        struct PcmSink final {
            const CancellationToken &cancellation;
            std::vector<std::uint8_t> payload;
            std::optional<AudioProcessingFormat> format;
            std::uint64_t frames{};

            // AudioDecodedFrameSink borrows this exact stack object; decoded blocks are copied before returning.
            static Result<void> Write(void *context, const std::uint64_t firstFrame, const AudioProcessingFormat &format,
                                      const std::span<const AudioSample> samples) {
                auto &self = *static_cast<PcmSink *>(context);
                if (self.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(AudioErrors::OperationCancelled));
                const auto channels = format.layout.orderedChannels.size();
                if (firstFrame != self.frames || channels == 0 || samples.size() % channels != 0 || (self.format && *self.format != format))
                    return Result<void>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
                if (samples.size() > (MaximumCookedAudioPayloadBytes - self.payload.size()) / sizeof(AudioSample))
                    return Result<void>::Failure(MakeError(AudioErrors::CookBudgetExceeded));
                if (!self.format)
                    self.format = format;
                for (const auto sample : samples) {
                    if (!std::isfinite(sample))
                        return Result<void>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
                    Append32(self.payload, std::bit_cast<std::uint32_t>(sample == 0.0F ? 0.0F : sample));
                }
                self.frames += samples.size() / channels;
                return Result<void>::Success();
            }
        };

        std::vector<std::uint8_t> EncodeHeader(const AudioCookManifest &manifest) {
            std::vector<std::uint8_t> bytes{'H', 'A', 'C', '1'};
            Append32(bytes, AudioCookSchemaVersion);
            bytes.push_back(static_cast<std::uint8_t>(manifest.residency));
            Append32(bytes, manifest.sourceContainer.value);
            Append32(bytes, manifest.sourceCodec.value);
            Append32(bytes, manifest.container.value);
            Append32(bytes, manifest.codec.value);
            bytes.push_back(static_cast<std::uint8_t>(manifest.compression));
            bytes.push_back(static_cast<std::uint8_t>(manifest.quality));
            bytes.push_back(manifest.targetOverride ? 1 : 0);
            Append32(bytes, manifest.format.sampleRate);
            AppendLayout(bytes, manifest.format.layout);
            Append64(bytes, manifest.frameCount);
            Append64(bytes, manifest.streamThresholdFrames);
            Append32(bytes, manifest.chunkFrames);
            Append32(bytes, manifest.chunkCount);
            Append32(bytes, manifest.encoderDelayFrames);
            Append32(bytes, manifest.encoderPaddingFrames);
            Append32(bytes, static_cast<std::uint32_t>(manifest.target.Value().size()));
            bytes.insert(bytes.end(), manifest.target.Value().begin(), manifest.target.Value().end());
            AppendDigest(bytes, manifest.sourceDigest);
            AppendDigest(bytes, manifest.decoderIdentityDigest);
            AppendDigest(bytes, manifest.configurationDigest);
            AppendDigest(bytes, manifest.toolchainDigest);
            AppendDigest(bytes, manifest.payloadDigest);
            Append64(bytes, manifest.payloadByteCount);
            const auto analysis = EncodeAnalysis(manifest.analysis);
            Append32(bytes, static_cast<std::uint32_t>(analysis.size()));
            AppendDigest(bytes, ComputeSha256(std::as_bytes(std::span{analysis})));
            bytes.insert(bytes.end(), analysis.begin(), analysis.end());
            const auto channels = manifest.format.layout.orderedChannels.size();
            const std::uint64_t bytesPerFrame = channels * sizeof(AudioSample);
            for (std::uint32_t index = 0; index < manifest.chunkCount; ++index) {
                const std::uint64_t firstFrame = static_cast<std::uint64_t>(index) * manifest.chunkFrames;
                Append64(bytes, firstFrame * bytesPerFrame);
                Append32(bytes,
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(manifest.chunkFrames, manifest.frameCount - firstFrame)));
            }
            return bytes;
        }

        /** @brief Collects the exact source, policy and toolchain facts serialized into compatibility output. */
        AudioCookManifest MakeManifest(const AudioCookPlan &plan, const AudioCookToolchain &toolchain,
                                       const std::span<const std::uint8_t> source, const std::span<const std::uint8_t> payload,
                                       const std::uint32_t chunkFrames, const std::uint32_t chunkCount,
                                       const AudioAnalysisMetadata &analysis) {
            return {
                .target = plan.target,
                .sourceContainer = plan.sourceContainer,
                .sourceCodec = plan.sourceCodec,
                .container = plan.settings.container,
                .codec = plan.settings.codec,
                .compression = plan.settings.compression,
                .quality = plan.settings.quality,
                .format = plan.outputFormat,
                .residency = plan.residency,
                .targetOverride = plan.targetOverride,
                .frameCount = plan.frameCount,
                .streamThresholdFrames = plan.settings.streamThresholdFrames,
                .chunkFrames = chunkFrames,
                .chunkCount = chunkCount,
                .encoderDelayFrames = plan.settings.encoderDelayFrames,
                .encoderPaddingFrames = plan.settings.encoderPaddingFrames,
                .payloadByteCount = payload.size(),
                .sourceDigest = ComputeSha256(std::as_bytes(source)),
                .decoderIdentityDigest = ComputeSha256(std::as_bytes(std::span{plan.decoderIdentity})),
                .configurationDigest = plan.configurationDigest,
                .toolchainDigest = ComputeSha256(std::as_bytes(std::span{toolchain.identity})),
                .payloadDigest = ComputeSha256(std::as_bytes(payload)),
                .analysis = analysis,
            };
        }

        class AudioCookerStrategy final : public Assets::ICookerStrategy {
        public:
            AudioCookerStrategy(AudioCookProfile profile, AssetCookTargetId target, AudioCookToolchain toolchain,
                                const Sha256Digest &configurationDigest)
                : profile_(std::move(profile)), target_(std::move(target)), toolchain_(std::move(toolchain)),
                  configurationDigest_(configurationDigest) {}

            [[nodiscard]] Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "1.0.0", .settingsDigest = configurationDigest_, .settingsSchemaVersion = AudioCookSchemaVersion};
            }

            [[nodiscard]] Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                                             const std::span<const std::uint8_t> payload) const override {
                auto inspected = InspectCookedAudio(payload);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                if (const auto &manifest = inspected.Value();
                    manifest.target != target_ || manifest.target != source.target || manifest.sourceDigest != source.sourceDigest ||
                    manifest.configurationDigest != configurationDigest_ ||
                    manifest.toolchainDigest != ComputeSha256(std::as_bytes(std::span{toolchain_.identity})))
                    return Result<void>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
                return Result<void>::Success();
            }

            [[nodiscard]] Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                              const CancellationToken &cancellation) const override {
                if (source.target != target_ || ComputeSha256(std::as_bytes(source.bytes)) != source.sourceDigest)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
                auto cooked = CookAudioSource(source.bytes, profile_, target_, toolchain_, cancellation);
                if (cooked.HasError())
                    return Result<Assets::CookOutputSink>::Failure(cooked.ErrorValue());
                Assets::CookOutputSink sink;
                sink.payload = std::move(cooked).Value().bytes;
                return Result<Assets::CookOutputSink>::Success(std::move(sink));
            }

        private:
            AudioCookProfile profile_;
            AssetCookTargetId target_;
            AudioCookToolchain toolchain_;
            Sha256Digest configurationDigest_;
        };
    }  // namespace

    /** @copydoc CookAudioSource */
    Result<AudioCookedOutput> CookAudioSource(const std::span<const std::uint8_t> source, const AudioCookProfile &profile,
                                              const AssetCookTargetId &target, const AudioCookToolchain &toolchain,
                                              const CancellationToken &cancellation) {
        if (auto configuration = FingerprintAudioCookConfiguration(profile, target, toolchain); configuration.HasError())
            return Result<AudioCookedOutput>::Failure(configuration.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<AudioCookedOutput>::Failure(MakeError(AudioErrors::OperationCancelled));
        MemoryReader reader{std::as_bytes(source)};
        PcmSink sink{cancellation};
        // ImportCoreAudioSource invokes both callbacks synchronously and retains neither borrowed context.
        auto imported = ImportCoreAudioSource({&reader, source.size(), &MemoryReader::Read}, {&sink, &PcmSink::Write});
        if (imported.HasError())
            return Result<AudioCookedOutput>::Failure(imported.ErrorValue());
        auto plan = ResolveAudioCookPlan(imported.Value(), profile, target, toolchain);
        if (plan.HasError())
            return Result<AudioCookedOutput>::Failure(plan.ErrorValue());
        if (sink.frames != plan.Value().frameCount || !sink.format || *sink.format != plan.Value().outputFormat)
            return Result<AudioCookedOutput>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        const auto &resolved = plan.Value();
        const auto chunkFrames = resolved.residency == AudioCookResidency::Streamed ? resolved.settings.streamChunkFrames
                                                                                    : static_cast<std::uint32_t>(resolved.frameCount);
        const auto chunks = 1 + (resolved.frameCount - 1) / chunkFrames;
        if (chunks > MaximumChunkCount)
            return Result<AudioCookedOutput>::Failure(MakeError(AudioErrors::CookBudgetExceeded));
        auto manifest = MakeManifest(resolved, toolchain, source, sink.payload, chunkFrames, static_cast<std::uint32_t>(chunks),
                                     imported.Value().analysis);
        auto bytes = EncodeHeader(manifest);
        if (bytes.size() > MaximumCookedAudioPayloadBytes - sink.payload.size())
            return Result<AudioCookedOutput>::Failure(MakeError(AudioErrors::CookBudgetExceeded));
        bytes.insert(bytes.end(), sink.payload.begin(), sink.payload.end());
        return Result<AudioCookedOutput>::Success({std::move(manifest), std::move(bytes)});
    }

    /** @copydoc MakeAudioCookerContribution */
    Result<Assets::CookerContribution> MakeAudioCookerContribution(const Assets::AssetTypeId &type, AudioCookProfile profile,
                                                                   AssetCookTargetId target, AudioCookToolchain toolchain) {
        if (type.Value().empty())
            return Result<Assets::CookerContribution>::Failure(MakeError(AudioErrors::CookProfileInvalid));
        auto configuration = FingerprintAudioCookConfiguration(profile, target, toolchain);
        if (configuration.HasError())
            return Result<Assets::CookerContribution>::Failure(configuration.ErrorValue());
        return Result<Assets::CookerContribution>::Success(Assets::CookerContribution{
            .contributionId = "horo.audio.pcm-cook",
            .assetType = type,
            .targets = {target},
            .strategy = std::make_shared<const AudioCookerStrategy>(std::move(profile), std::move(target), std::move(toolchain),
                                                                    configuration.Value()),
        });
    }
}  // namespace Horo::Audio
