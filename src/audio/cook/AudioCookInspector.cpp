#include "Horo/Audio/AudioCooker.h"
#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Audio {
    namespace {
        class Reader final {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool Take(const std::size_t count, std::span<const std::uint8_t> &out) noexcept {
                if (count > bytes_.size() - cursor_)
                    return false;
                out = bytes_.subspan(cursor_, count);
                cursor_ += count;
                return true;
            }

            [[nodiscard]] bool U8(std::uint8_t &out) noexcept {
                std::span<const std::uint8_t> bytes;
                if (!Take(1, bytes))
                    return false;
                out = bytes[0];
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &out) noexcept {
                std::span<const std::uint8_t> bytes;
                if (!Take(4, bytes))
                    return false;
                out = 0;
                for (unsigned index = 0; index < 4; ++index)
                    out |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t &out) noexcept {
                std::span<const std::uint8_t> bytes;
                if (!Take(8, bytes))
                    return false;
                out = 0;
                for (unsigned index = 0; index < 8; ++index)
                    out |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
                return true;
            }

            [[nodiscard]] bool Float(float &out) noexcept {
                std::uint32_t bits{};
                if (!U32(bits))
                    return false;
                out = std::bit_cast<float>(bits);
                return std::isfinite(out);
            }

            [[nodiscard]] bool Digest(Sha256Digest &out) noexcept {
                std::span<const std::uint8_t> bytes;
                if (!Take(out.bytes.size(), bytes))
                    return false;
                std::ranges::copy(bytes, out.bytes.begin());
                return true;
            }

            [[nodiscard]] std::size_t Remaining() const noexcept {
                return bytes_.size() - cursor_;
            }

        private:
            std::span<const std::uint8_t> bytes_;
            std::size_t cursor_{};
        };

        [[nodiscard]] bool ReadOptionalFloat(Reader &reader, std::optional<float> &out) noexcept {
            std::uint8_t present{};
            if (!reader.U8(present) || present > 1)
                return false;
            if (present) {
                float value{};
                if (!reader.Float(value))
                    return false;
                out = value;
            }
            return true;
        }

        [[nodiscard]] bool ReadWaveformPoint(Reader &reader, AudioWaveformPoint &point, const std::uint64_t nextFrame,
                                             const std::uint64_t frameCount, const std::uint64_t windowFrames, const bool finalPoint) {
            if (!reader.U64(point.firstFrame) || !reader.U32(point.frameCount) || !reader.Float(point.minimum) ||
                !reader.Float(point.maximum) || point.firstFrame != nextFrame || nextFrame > frameCount || point.frameCount == 0 ||
                point.frameCount > windowFrames || point.frameCount > frameCount - nextFrame || point.minimum > point.maximum)
                return false;
            return finalPoint || point.frameCount == windowFrames;
        }

        [[nodiscard]] bool ReadWaveformLevel(Reader &reader, AudioWaveformLevel &level, const AudioWaveformLevel *previous,
                                             const std::uint64_t frameCount, std::uint64_t &pointCountTotal, AudioSample &waveformPeak) {
            std::uint32_t pointCount{};
            if (!reader.U64(level.windowFrames) || level.windowFrames == 0 || !reader.U32(pointCount) || pointCount == 0 ||
                pointCount > 524'288 || pointCountTotal + pointCount > 1'048'576 || pointCount > reader.Remaining() / 20U)
                return false;
            pointCountTotal += pointCount;
            if (!previous &&
                (level.windowFrames > std::numeric_limits<std::uint32_t>::max() || pointCount != 1 + (frameCount - 1) / level.windowFrames))
                return false;
            if (previous && (previous->windowFrames > std::numeric_limits<std::uint64_t>::max() / 2 ||
                             level.windowFrames != previous->windowFrames * 2 || pointCount != (previous->points.size() + 1) / 2))
                return false;
            level.points.reserve(pointCount);
            std::uint64_t nextFrame{};
            for (std::uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
                AudioWaveformPoint point;
                if (!ReadWaveformPoint(reader, point, nextFrame, frameCount, level.windowFrames, pointIndex + 1 == pointCount))
                    return false;
                nextFrame += point.frameCount;
                if (!previous) {
                    waveformPeak = std::max({waveformPeak, std::abs(point.minimum), std::abs(point.maximum)});
                } else {
                    const auto &first = previous->points[static_cast<std::size_t>(pointIndex) * 2];
                    const auto &second =
                        previous->points[std::min<std::size_t>(static_cast<std::size_t>(pointIndex) * 2 + 1, previous->points.size() - 1)];
                    const auto expectedFrames = first.frameCount + (pointIndex * 2 + 1 < previous->points.size() ? second.frameCount : 0);
                    if (point.firstFrame != first.firstFrame || point.frameCount != expectedFrames ||
                        point.minimum != std::min(first.minimum, second.minimum) ||
                        point.maximum != std::max(first.maximum, second.maximum))
                        return false;
                }
                level.points.push_back(point);
            }
            return nextFrame == frameCount;
        }

        [[nodiscard]] bool ReadAnalysis(Reader &reader, AudioCookManifest &manifest) {
            if (manifest.frameCount == 0 || manifest.payloadByteCount == 0)
                return false;
            std::uint32_t byteCount{};
            Sha256Digest digest;
            if (!reader.U32(byteCount) || byteCount > MaximumCookedAudioPayloadBytes || !reader.Digest(digest))
                return false;
            std::span<const std::uint8_t> encoded;
            if (!reader.Take(byteCount, encoded) || ComputeSha256(std::as_bytes(encoded)) != digest)
                return false;
            Reader analysis{encoded};
            auto &result = manifest.analysis;
            std::uint32_t version{};
            std::uint32_t levelCount{};
            if (!analysis.U32(version) || version != 1 || !analysis.U64(result.residentPcmBytes) ||
                !analysis.U64(result.decodeBlockBytes) || !analysis.U64(result.waveformBytes) || !analysis.Float(result.samplePeak) ||
                result.samplePeak < 0.0F || !ReadOptionalFloat(analysis, result.loudness.integratedLufs) ||
                !ReadOptionalFloat(analysis, result.loudness.shortTermLufs) || !ReadOptionalFloat(analysis, result.loudness.truePeakDbtp) ||
                !ReadOptionalFloat(analysis, result.loudness.rmsDbfs) ||
                !ReadOptionalFloat(analysis, result.loudness.normalizationGainDb) || !analysis.U32(levelCount) || levelCount == 0 ||
                levelCount > 32 || result.residentPcmBytes != manifest.payloadByteCount || result.decodeBlockBytes == 0 ||
                result.decodeBlockBytes > 4'096U * MaximumCoreAudioSourceChannels * sizeof(AudioSample))
                return false;
            if (result.loudness.normalizationGainDb.has_value() && !result.loudness.integratedLufs.has_value())
                return false;
            result.waveformLevels.reserve(levelCount);
            std::uint64_t pointCountTotal{};
            AudioSample waveformPeak{};
            for (std::uint32_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
                AudioWaveformLevel level;
                if (const auto *previous = result.waveformLevels.empty() ? nullptr : &result.waveformLevels.back();
                    !ReadWaveformLevel(analysis, level, previous, manifest.frameCount, pointCountTotal, waveformPeak))
                    return false;
                result.waveformLevels.push_back(std::move(level));
            }
            return analysis.Remaining() == 0 && result.waveformLevels.back().points.size() == 1 &&
                   result.waveformBytes == pointCountTotal * 20U && waveformPeak == result.samplePeak;
        }

        [[nodiscard]] bool ReadLayout(Reader &reader, AudioChannelLayout &layout) {
            std::uint8_t kind{};
            std::uint8_t channels{};
            std::uint8_t ambisonicOrder{};
            if (!reader.U8(kind) || !reader.U8(channels) || !reader.U8(ambisonicOrder) ||
                kind > static_cast<std::uint8_t>(AudioLayoutKind::Discrete) || channels == 0 || channels > MaximumAudioChannels)
                return false;
            layout.kind = static_cast<AudioLayoutKind>(kind);
            if (layout.kind == AudioLayoutKind::Ambisonic)
                layout.ambisonic = AmbisonicDescriptor{ambisonicOrder};
            else if (ambisonicOrder != 0)
                return false;
            layout.orderedChannels.reserve(channels);
            for (std::uint8_t index = 0; index < channels; ++index) {
                std::uint8_t family{};
                std::uint8_t value{};
                if (!reader.U8(family) || !reader.U8(value))
                    return false;
                switch (family) {
                    case 0:
                        if (value > static_cast<std::uint8_t>(AudioSpeakerRole::TopBackRight))
                            return false;
                        layout.orderedChannels.emplace_back(static_cast<AudioSpeakerRole>(value));
                        break;
                    case 1:
                        layout.orderedChannels.emplace_back(AudioDiscreteChannel{value});
                        break;
                    case 2:
                        layout.orderedChannels.emplace_back(AudioAmbisonicChannel{value});
                        break;
                    default:
                        return false;
                }
            }
            return ValidateAudioChannelLayout(ViewAudioChannelLayout(layout));
        }

        [[nodiscard]] bool ReadManifestFields(Reader &reader, AudioCookManifest &manifest) {
            std::uint32_t version{};
            std::uint32_t sourceContainer{};
            std::uint32_t sourceCodec{};
            std::uint32_t container{};
            std::uint32_t codec{};
            std::uint32_t targetBytes{};
            std::uint8_t residency{};
            std::uint8_t compression{};
            std::uint8_t quality{};
            std::uint8_t targetOverride{};
            if (!reader.U32(version) || version != AudioCookSchemaVersion || !reader.U8(residency) ||
                residency < static_cast<std::uint8_t>(AudioCookResidency::Resident) ||
                residency > static_cast<std::uint8_t>(AudioCookResidency::Streamed) || !reader.U32(sourceContainer) ||
                !reader.U32(sourceCodec) || !reader.U32(container) || container != AudioContainerIds::HoroCooked.value ||
                !reader.U32(codec) || codec != AudioCodecIds::Pcm.value || !reader.U8(compression) ||
                compression != static_cast<std::uint8_t>(AudioCookCompression::None) || !reader.U8(quality) ||
                quality != static_cast<std::uint8_t>(AudioCookQuality::Float32Exact) || !reader.U8(targetOverride) || targetOverride > 1 ||
                !reader.U32(manifest.format.sampleRate) || !ReadLayout(reader, manifest.format.layout) ||
                !reader.U64(manifest.frameCount) || !reader.U64(manifest.streamThresholdFrames) || !reader.U32(manifest.chunkFrames) ||
                !reader.U32(manifest.chunkCount) || !reader.U32(manifest.encoderDelayFrames) ||
                !reader.U32(manifest.encoderPaddingFrames) || !reader.U32(targetBytes) || targetBytes > MaximumAssetCookTargetIdBytes)
                return false;
            std::span<const std::uint8_t> targetText;
            if (!reader.Take(targetBytes, targetText))
                return false;
            const auto target = AssetCookTargetId::Parse({reinterpret_cast<const char *>(targetText.data()), targetText.size()});
            if (target.HasError())
                return false;
            manifest.target = target.Value();
            manifest.sourceContainer = AudioContainerId{sourceContainer};
            manifest.sourceCodec = AudioCodecId{sourceCodec};
            manifest.container = AudioContainerIds::HoroCooked;
            manifest.codec = AudioCodecIds::Pcm;
            manifest.compression = AudioCookCompression::None;
            manifest.quality = AudioCookQuality::Float32Exact;
            manifest.residency = static_cast<AudioCookResidency>(residency);
            manifest.targetOverride = targetOverride != 0;
            return reader.Digest(manifest.sourceDigest) && reader.Digest(manifest.decoderIdentityDigest) &&
                   reader.Digest(manifest.configurationDigest) && reader.Digest(manifest.toolchainDigest) &&
                   reader.Digest(manifest.payloadDigest) && reader.U64(manifest.payloadByteCount);
        }

        [[nodiscard]] bool ValidateChunks(Reader &reader, const AudioCookManifest &manifest) {
            if (!((manifest.sourceContainer == AudioContainerIds::Wave && manifest.sourceCodec == AudioCodecIds::Pcm) ||
                  (manifest.sourceContainer == AudioContainerIds::Ogg && manifest.sourceCodec == AudioCodecIds::Vorbis)) ||
                manifest.frameCount == 0 || manifest.streamThresholdFrames == 0 || manifest.chunkFrames == 0 || manifest.chunkCount == 0 ||
                manifest.chunkCount > 65'536 || manifest.encoderDelayFrames != 0 || manifest.encoderPaddingFrames != 0 ||
                !ValidateAudioProcessingFormat(manifest.format) || manifest.payloadByteCount > MaximumCookedAudioPayloadBytes)
                return false;
            const auto channels = manifest.format.layout.orderedChannels.size();
            const std::uint64_t bytesPerFrame = channels * sizeof(AudioSample);
            if (manifest.frameCount > MaximumCookedAudioPayloadBytes / bytesPerFrame ||
                manifest.payloadByteCount != manifest.frameCount * bytesPerFrame ||
                manifest.chunkCount != 1 + (manifest.frameCount - 1) / manifest.chunkFrames ||
                (manifest.residency == AudioCookResidency::Streamed && manifest.chunkFrames > MaximumAudioStreamChunkFrames) ||
                (manifest.residency == AudioCookResidency::Resident &&
                 (manifest.chunkCount != 1 || manifest.chunkFrames != manifest.frameCount)))
                return false;
            for (std::uint32_t index = 0; index < manifest.chunkCount; ++index) {
                std::uint64_t offset{};
                std::uint32_t frames{};
                const std::uint64_t firstFrame = static_cast<std::uint64_t>(index) * manifest.chunkFrames;
                if (!reader.U64(offset) || !reader.U32(frames) || offset != firstFrame * bytesPerFrame ||
                    frames != std::min<std::uint64_t>(manifest.chunkFrames, manifest.frameCount - firstFrame))
                    return false;
            }
            return true;
        }

        /** @brief Rejects non-finite PCM even if an untrusted artifact supplied a matching digest. */
        [[nodiscard]] bool ValidatePcmSamples(const std::span<const std::uint8_t> payload) noexcept {
            for (std::size_t offset = 0; offset < payload.size(); offset += sizeof(AudioSample)) {
                const std::uint32_t bits =
                    static_cast<std::uint32_t>(payload[offset]) | (static_cast<std::uint32_t>(payload[offset + 1]) << 8U) |
                    (static_cast<std::uint32_t>(payload[offset + 2]) << 16U) | (static_cast<std::uint32_t>(payload[offset + 3]) << 24U);
                if (!std::isfinite(std::bit_cast<AudioSample>(bits)))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc InspectCookedAudio */
    Result<AudioCookManifest> InspectCookedAudio(const std::span<const std::uint8_t> bytes) {
        if (bytes.size() > MaximumCookedAudioPayloadBytes || bytes.size() < 4)
            return Result<AudioCookManifest>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        Reader reader{bytes};
        if (std::span<const std::uint8_t> magic;
            !reader.Take(4, magic) || !std::ranges::equal(magic, std::array<std::uint8_t, 4>{'H', 'A', 'C', '1'}))
            return Result<AudioCookManifest>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        AudioCookManifest manifest;
        if (!ReadManifestFields(reader, manifest) || !ReadAnalysis(reader, manifest) || !ValidateChunks(reader, manifest) ||
            reader.Remaining() != manifest.payloadByteCount)
            return Result<AudioCookManifest>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        if (std::span<const std::uint8_t> payload; !reader.Take(static_cast<std::size_t>(manifest.payloadByteCount), payload) ||
                                                   ComputeSha256(std::as_bytes(payload)) != manifest.payloadDigest ||
                                                   !ValidatePcmSamples(payload))
            return Result<AudioCookManifest>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        return Result<AudioCookManifest>::Success(std::move(manifest));
    }
}  // namespace Horo::Audio
