#include "Horo/Audio/AudioCooker.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioStreamDecoderErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace Horo::Audio {
    namespace {
        /** @brief Immutable host-selected provider generation retained by source and fill jobs. */
        struct CookedSource final {
            std::shared_ptr<const Assets::IAssetProvider> provider;
            AssetCookTargetId target;
            Assets::AssetTypeId type;
            std::size_t maximumArtifactBytes{};

            CookedSource(std::shared_ptr<const Assets::IAssetProvider> value, AssetCookTargetId cookTarget, Assets::AssetTypeId assetType,
                         const std::size_t limit)
                : provider(std::move(value)), target(std::move(cookTarget)), type(std::move(assetType)), maximumArtifactBytes(limit) {}
        };

        /** @brief One worker-owned verified PCM generation, released after its last decode joins. */
        struct CookedDecoder final {
            std::vector<std::uint8_t> payload;
            std::size_t pcmOffset{};
            std::size_t channels{};
            std::uint64_t frames{};
        };

        /** @brief Converts one bounded little-endian cooked PCM block into worker-owned samples. */
        Result<AudioStreamDecodeProgress> DecodePcm(const BorrowedCallbackContext &context, const std::uint64_t firstFrame,
                                                    const std::span<AudioSample> output, std::span<std::byte>,
                                                    const std::atomic<bool> &cancelled) {
            const auto *resolved = context.Get<CookedDecoder>();
            if (resolved == nullptr)
                return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
            const auto &state = *resolved;
            const auto frames =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(output.size() / state.channels, state.frames - firstFrame));
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                // Worker-side polling follows the decoder session's SC cancellation contract;
                // no callback ring or real-time operations use this path.
                if (cancelled.load())
                    return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::OperationCancelled));
                for (std::size_t channel = 0; channel < state.channels; ++channel) {
                    const auto offset = state.pcmOffset + ((firstFrame + frame) * state.channels + channel) * sizeof(AudioSample);
                    const auto *bytes = state.payload.data() + offset;
                    const std::uint32_t bits = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                               (static_cast<std::uint32_t>(bytes[3]) << 24U);
                    const auto sample = std::bit_cast<AudioSample>(bits);
                    output[static_cast<std::size_t>(frame) * state.channels + channel] =
                        sample == 0.0F || std::fpclassify(sample) == FP_SUBNORMAL ? 0.0F : sample;
                }
            }
            return Result<AudioStreamDecodeProgress>::Success({frames, firstFrame + frames == state.frames});
        }

        /** @brief PCM random access needs no private cursor or I/O. */
        Result<void> SeekPcm(const BorrowedCallbackContext &context, std::uint64_t, const std::atomic<bool> &cancelled) {
            if (context.Get<CookedDecoder>() == nullptr)
                return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
            if (cancelled.load())
                return Result<void>::Failure(MakeError(AudioErrors::OperationCancelled));
            return Result<void>::Success();
        }

        /** @brief Releases the immutable payload only after serialized worker use ends. */
        void ReleasePcm(const BorrowedCallbackContext &context) noexcept {
            std::unique_ptr<CookedDecoder> state(context.Get<CookedDecoder>());
            state.reset();
        }

        /** @brief Loads a provider-bounded artifact and validates its exact envelope identity. */
        Result<Assets::AssetCookArtifact> LoadArtifact(const CookedSource &source, const Assets::AssetId asset,
                                                       const CancellationToken &cancellation) {
            auto loaded = source.provider->Load(asset, cancellation);
            if (loaded.HasError())
                return Result<Assets::AssetCookArtifact>::Failure(std::move(loaded).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(AudioErrors::OperationCancelled));
            if (loaded.Value().size() > source.maximumArtifactBytes)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
            Assets::AssetCookLimits limits;
            limits.maximumArtifactBytes = source.maximumArtifactBytes;
            auto encoded = std::move(loaded).Value();
            auto artifact = Assets::DecodeCookedArtifact(encoded, limits);
            if (artifact.HasError())
                return Result<Assets::AssetCookArtifact>::Failure(std::move(artifact).ErrorValue());
            std::vector<std::uint8_t>().swap(encoded);
            if (const auto &envelope = artifact.Value();
                envelope.id != asset || envelope.target != source.target || envelope.type != source.type)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
            return artifact;
        }

        /** @brief Checks cooked residency and decoder compatibility after payload inspection. */
        bool MatchesManifest(const AudioCookManifest &manifest, const CookedSource &source, const AudioStreamDecoderSpec &expected) {
            return manifest.target == source.target && manifest.codec == expected.codec && manifest.format == expected.outputFormat &&
                   manifest.frameCount == expected.frameCount && manifest.residency == AudioCookResidency::Streamed &&
                   expected.requiredWorkingBytes == 0;
        }

        /** @brief Transfers verified PCM ownership to the existing decoder contract. */
        Result<AudioStreamDecoder> CreateDecoder(Assets::AssetCookArtifact artifact, const AudioCookManifest &manifest,
                                                 const AudioStreamDecoderSpec &expected) {
            auto state = std::make_unique<CookedDecoder>();
            state->pcmOffset = artifact.payload.size() - static_cast<std::size_t>(manifest.payloadByteCount);
            state->channels = manifest.format.layout.orderedChannels.size();
            state->frames = manifest.frameCount;
            state->payload = std::move(artifact.payload);
            const AudioStreamDecoderProvider provider{BorrowedCallbackContext{state.get()}, &DecodePcm,
                                                      expected.seekable ? &SeekPcm : nullptr, &ReleasePcm};
            AudioStreamDecoderLimits decoderLimits;
            decoderLimits.maximumFramesPerDecode = MaximumAudioCallbackFrames;
            auto decoder = AudioStreamDecoder::Create(expected, provider, decoderLimits);
            if (decoder.HasValue())
                (void)state.release();
            return decoder;
        }

        /** @brief Loads and verifies an exact cooked generation on a cancellable worker. */
        Result<AudioStreamDecoder> OpenCooked(const BorrowedCallbackContext &context, const Assets::AssetId asset,
                                              const AudioStreamDecoderSpec &expected, const std::size_t maximumPackageBytes,
                                              const CancellationToken &cancellation) {
            const auto *resolved = context.Get<CookedSource>();
            if (resolved == nullptr)
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::StreamReadFailed));
            const auto &source = *resolved;
            if (maximumPackageBytes < source.maximumArtifactBytes * 3U)
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
            auto artifact = LoadArtifact(source, asset, cancellation);
            if (artifact.HasError())
                return Result<AudioStreamDecoder>::Failure(std::move(artifact).ErrorValue());
            const auto &envelope = artifact.Value();
            auto inspected = InspectCookedAudio(envelope.payload);
            if (inspected.HasError())
                return Result<AudioStreamDecoder>::Failure(std::move(inspected).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::OperationCancelled));
            const auto &manifest = inspected.Value();
            if (!MatchesManifest(manifest, source, expected))
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
            return CreateDecoder(std::move(artifact).Value(), manifest, expected);
        }
    }  // namespace

    /** @copydoc MakeCookedAudioStreamSource */
    Result<AudioStreamPackageSource> MakeCookedAudioStreamSource(std::shared_ptr<const Assets::IAssetProvider> provider,
                                                                 AssetCookTargetId target, Assets::AssetTypeId type,
                                                                 const std::size_t maximumArtifactBytes) {
        if (!provider || !target.IsValid() || type.Value().empty() || maximumArtifactBytes == 0 ||
            maximumArtifactBytes > (128U << 20U) / 3U)
            return Result<AudioStreamPackageSource>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        auto state = std::make_shared<CookedSource>(std::move(provider), std::move(target), std::move(type), maximumArtifactBytes);
        return Result<AudioStreamPackageSource>::Success({BorrowedCallbackContext{state.get()}, &OpenCooked, std::move(state)});
    }
}  // namespace Horo::Audio
