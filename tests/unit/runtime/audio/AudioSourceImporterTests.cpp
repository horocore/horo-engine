#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioSourceImporter.h"
#include "OggVorbisFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Horo::Audio {
    namespace {
        template <typename Integer> void AppendLittle(std::vector<std::byte> &bytes, const Integer value) {
            for (std::size_t index = 0; index < sizeof(Integer); ++index)
                bytes.push_back(static_cast<std::byte>((static_cast<std::make_unsigned_t<Integer>>(value) >> (index * 8U)) & 0xFFU));
        }

        void AppendFour(std::vector<std::byte> &bytes, const char (&value)[5]) {
            for (std::size_t index = 0; index < 4; ++index)
                bytes.push_back(static_cast<std::byte>(value[index]));
        }

        void StoreLittle32(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
            for (std::size_t index = 0; index < sizeof(value); ++index)
                bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }

        std::uint8_t DecodeBase64Digit(const char digit) {
            if (digit >= 'A' && digit <= 'Z')
                return static_cast<std::uint8_t>(digit - 'A');
            if (digit >= 'a' && digit <= 'z')
                return static_cast<std::uint8_t>(digit - 'a' + 26);
            if (digit >= '0' && digit <= '9')
                return static_cast<std::uint8_t>(digit - '0' + 52);
            return digit == '+' ? 62 : 63;
        }

        std::vector<std::byte> DecodeBase64(const std::string_view encoded) {
            REQUIRE(encoded.size() % 4 == 0);
            std::vector<std::byte> bytes;
            bytes.reserve(encoded.size() / 4 * 3);
            for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
                const std::uint32_t value =
                    static_cast<std::uint32_t>(DecodeBase64Digit(encoded[offset])) << 18U |
                    static_cast<std::uint32_t>(DecodeBase64Digit(encoded[offset + 1])) << 12U |
                    static_cast<std::uint32_t>(encoded[offset + 2] == '=' ? 0 : DecodeBase64Digit(encoded[offset + 2])) << 6U |
                    static_cast<std::uint32_t>(encoded[offset + 3] == '=' ? 0 : DecodeBase64Digit(encoded[offset + 3]));
                bytes.push_back(static_cast<std::byte>(value >> 16U));
                if (encoded[offset + 2] != '=')
                    bytes.push_back(static_cast<std::byte>(value >> 8U));
                if (encoded[offset + 3] != '=')
                    bytes.push_back(static_cast<std::byte>(value));
            }
            return bytes;
        }

        std::vector<std::byte> WaveFixture(const bool includeLoop = true) {
            constexpr std::array<std::int16_t, 8> samples{0, 16'384, 32'767, -32'768, -16'384, 8'192, 0, -8'192};
            std::vector<std::byte> bytes;
            AppendFour(bytes, "RIFF");
            AppendLittle<std::uint32_t>(bytes, 0);
            AppendFour(bytes, "WAVE");
            AppendFour(bytes, "fmt ");
            AppendLittle<std::uint32_t>(bytes, 16);
            AppendLittle<std::uint16_t>(bytes, 1);
            AppendLittle<std::uint16_t>(bytes, 2);
            AppendLittle<std::uint32_t>(bytes, 48'000);
            AppendLittle<std::uint32_t>(bytes, 48'000 * 4);
            AppendLittle<std::uint16_t>(bytes, 4);
            AppendLittle<std::uint16_t>(bytes, 16);
            if (includeLoop) {
                AppendFour(bytes, "smpl");
                AppendLittle<std::uint32_t>(bytes, 60);
                for (int index = 0; index < 7; ++index)
                    AppendLittle<std::uint32_t>(bytes, 0);
                AppendLittle<std::uint32_t>(bytes, 1);
                AppendLittle<std::uint32_t>(bytes, 0);
                AppendLittle<std::uint32_t>(bytes, 1);
                AppendLittle<std::uint32_t>(bytes, 0);
                AppendLittle<std::uint32_t>(bytes, 1);
                AppendLittle<std::uint32_t>(bytes, 2);
                AppendLittle<std::uint32_t>(bytes, 0);
                AppendLittle<std::uint32_t>(bytes, 0);
            }
            AppendFour(bytes, "data");
            AppendLittle<std::uint32_t>(bytes, static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t)));
            for (const auto sample : samples)
                AppendLittle(bytes, sample);
            StoreLittle32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
            return bytes;
        }

        struct MemoryReader final {
            std::span<const std::byte> bytes;
            std::uint32_t calls{};

            static Result<std::size_t> Read(void *context, const std::uint64_t offset, const std::span<std::byte> destination) {
                auto &self = *static_cast<MemoryReader *>(context);
                ++self.calls;
                if (offset > self.bytes.size())
                    return Result<std::size_t>::Failure(MakeError(AudioErrors::SourceReadFailed));
                const auto count = std::min(destination.size(), self.bytes.size() - static_cast<std::size_t>(offset));
                std::ranges::copy(self.bytes.subspan(static_cast<std::size_t>(offset), count), destination.begin());
                return Result<std::size_t>::Success(count);
            }
        };

        struct SampleSink final {
            std::vector<AudioSample> samples;
            std::vector<std::uint64_t> offsets;

            static Result<void> Write(void *context, const std::uint64_t firstFrame, const AudioProcessingFormat &,
                                      const std::span<const AudioSample> block) {
                auto &self = *static_cast<SampleSink *>(context);
                self.offsets.push_back(firstFrame);
                self.samples.insert(self.samples.end(), block.begin(), block.end());
                return Result<void>::Success();
            }
        };

        Result<AudioSourceImportCandidate> Import(std::vector<std::byte> &bytes, SampleSink &sink,
                                                  const AudioSourceImportLimits &limits = {}) {
            MemoryReader reader{bytes};
            return ImportCoreAudioSource({&reader, bytes.size(), &MemoryReader::Read}, {&sink, &SampleSink::Write}, limits);
        }

        template <typename Value> bool HasErrorCode(const Result<Value> &result, const ErrorCodeDescriptor &descriptor) {
            if (!result.HasError())
                return false;
            const auto &error = result.ErrorValue();
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }
    }  // namespace

    TEST_CASE("WAV import deterministically decodes frames loops waveform and loudness", "[unit][audio][import]") {
        auto bytes = WaveFixture();
        SampleSink firstSink;
        AudioSourceImportLimits limits;
        limits.decodeBlockFrames = 2;
        limits.waveformWindowFrames = 2;
        auto first = Import(bytes, firstSink, limits);
        REQUIRE(first.HasValue());

        SampleSink secondSink;
        auto second = Import(bytes, secondSink, limits);
        REQUIRE(second.HasValue());
        CHECK(first.Value() == second.Value());
        CHECK(firstSink.samples == secondSink.samples);
        CHECK(firstSink.offsets == std::vector<std::uint64_t>{0, 2});
        CHECK(first.Value().container == AudioContainerIds::Wave);
        CHECK(first.Value().codec == AudioCodecIds::Pcm);
        REQUIRE(first.Value().sourcePcm.has_value());
        CHECK(first.Value().sourcePcm->encoding == AudioPcmEncoding::SignedInteger);
        CHECK(first.Value().decodedFormat.sampleRate == 48'000);
        CHECK(first.Value().decodedFormat.layout == MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo));
        CHECK(first.Value().frameCount == 4);
        CHECK(first.Value().durationNanoseconds == 83'333);
        REQUIRE(first.Value().loops.size() == 1);
        CHECK(first.Value().loops.front().startFrame == 1);
        CHECK(first.Value().loops.front().endFrame == 3);
        REQUIRE(first.Value().waveform.size() == 2);
        CHECK(first.Value().waveform.front().minimum == Catch::Approx(-1.0F));
        CHECK(first.Value().waveform.front().maximum == Catch::Approx(32'767.0F / 32'768.0F));
        CHECK(first.Value().samplePeak == Catch::Approx(1.0F));
        REQUIRE(first.Value().loudness.rmsDbfs.has_value());
        CHECK(std::isfinite(*first.Value().loudness.rmsDbfs));
    }

    TEST_CASE("Audio import rejects malformed and hostile sources before publishing samples", "[unit][audio][import]") {
        auto bytes = WaveFixture(false);
        SampleSink sink;

        auto truncated = bytes;
        truncated.pop_back();
        CHECK(HasErrorCode(Import(truncated, sink), AudioErrors::SourceInvalid));
        CHECK(sink.samples.empty());

        AudioSourceImportLimits sourceLimit;
        sourceLimit.maximumSourceBytes = bytes.size() - 1;
        sourceLimit.maximumCumulativeReadBytes = sourceLimit.maximumSourceBytes;
        CHECK(HasErrorCode(Import(bytes, sink, sourceLimit), AudioErrors::SourceLimitExceeded));
        CHECK(sink.samples.empty());

        AudioSourceImportLimits decodedLimit;
        decodedLimit.maximumDecodedFrames = 3;
        CHECK(HasErrorCode(Import(bytes, sink, decodedLimit), AudioErrors::SourceLimitExceeded));
        CHECK(sink.samples.empty());

        StoreLittle32(bytes, 40, std::numeric_limits<std::uint32_t>::max());
        CHECK(HasErrorCode(Import(bytes, sink), AudioErrors::SourceInvalid));
        CHECK(sink.samples.empty());
    }

    TEST_CASE("Ogg Vorbis import deterministically decodes a known source", "[unit][audio][import]") {
        auto bytes = DecodeBase64(TestFixtures::OggVorbisSilenceBase64);
        SampleSink firstSink;
        auto first = Import(bytes, firstSink);
        REQUIRE(first.HasValue());

        SampleSink secondSink;
        auto second = Import(bytes, secondSink);
        REQUIRE(second.HasValue());
        CHECK(first.Value() == second.Value());
        CHECK(firstSink.samples == secondSink.samples);
        CHECK(first.Value().container == AudioContainerIds::Ogg);
        CHECK(first.Value().codec == AudioCodecIds::Vorbis);
        CHECK_FALSE(first.Value().sourcePcm.has_value());
        CHECK(first.Value().decodedFormat.sampleRate == 44'100);
        CHECK(first.Value().decodedFormat.layout == MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo));
        CHECK(first.Value().frameCount == 44'160);
        CHECK(first.Value().durationNanoseconds == 1'001'360'544);
        CHECK(first.Value().loops.empty());
        CHECK(first.Value().samplePeak == 0.0F);
        CHECK(first.Value().waveform.size() == 22);
    }
}  // namespace Horo::Audio
