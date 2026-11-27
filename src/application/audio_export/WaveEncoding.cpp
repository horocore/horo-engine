#include "WaveEncoding.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/OfflineMixerSession.h"
#include "Horo/Security/SecureMemory.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::uint64_t WaveHeaderBytes = 80;
        static_assert(sizeof(Audio::AudioSample) == 4 && std::numeric_limits<Audio::AudioSample>::is_iec559);

        /** @brief WAVE interchange bit positions are explicit; Horo enum integers are never serialized as native masks. */
        std::uint32_t SpeakerBit(const Audio::AudioSpeakerRole role) noexcept {
            using enum Audio::AudioSpeakerRole;
            constexpr std::array roles{std::pair{FrontLeft, 0x1U},       std::pair{FrontRight, 0x2U},
                                       std::pair{FrontCenter, 0x4U},     std::pair{LowFrequency, 0x8U},
                                       std::pair{BackLeft, 0x10U},       std::pair{BackRight, 0x20U},
                                       std::pair{SideLeft, 0x200U},      std::pair{SideRight, 0x400U},
                                       std::pair{TopFrontLeft, 0x1000U}, std::pair{TopFrontRight, 0x4000U},
                                       std::pair{TopBackLeft, 0x8000U},  std::pair{TopBackRight, 0x20000U}};
            const auto found = std::ranges::find(roles, role, &decltype(roles)::value_type::first);
            return found == roles.end() ? 0 : found->second;
        }

        /** @brief Checks the fixed WAVE and host byte limits before any PCM is allocated. */
        bool FitsWaveBudget(const std::uint64_t frames, const std::size_t bytesPerFrame, const std::uint64_t maximumBytes) noexcept {
            return frames != 0 && maximumBytes >= WaveHeaderBytes && maximumBytes <= Audio::Backend::MaximumOfflineAudioOutputBytes &&
                   frames <= (maximumBytes - WaveHeaderBytes) / bytesPerFrame;
        }
    }  // namespace

    /** @copydoc PrepareWaveShape */
    std::optional<WaveShape> PrepareWaveShape(const Audio::AudioProcessingFormat &format, const std::uint64_t frames,
                                              const std::uint64_t maximumEncodedBytes) noexcept {
        if (!Audio::ValidateAudioProcessingFormat(format) || format.layout.kind != Audio::AudioLayoutKind::Speaker)
            return std::nullopt;
        WaveShape shape;
        shape.count = static_cast<std::uint32_t>(format.layout.orderedChannels.size());
        shape.frames = frames;
        const auto bytesPerFrame = shape.count * sizeof(Audio::AudioSample);
        if (!FitsWaveBudget(frames, bytesPerFrame, maximumEncodedBytes))
            return std::nullopt;
        shape.encodedBytes = WaveHeaderBytes + shape.frames * bytesPerFrame;
        for (std::size_t index = 0; index < shape.count; ++index) {
            const auto *role = std::get_if<Audio::AudioSpeakerRole>(&format.layout.orderedChannels[index]);
            if (!role)
                return std::nullopt;
            const auto bit = SpeakerBit(*role);
            if (!bit || (shape.mask & bit))
                return std::nullopt;
            shape.mask |= bit;
            shape.channels[index] = {bit, index};
        }
        std::sort(shape.channels.begin(), shape.channels.begin() + shape.count);
        return shape;
    }

    namespace {
        /** @brief Write one unsigned little-endian integer into already sized file storage. */
        void Integer(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value,
                     const std::size_t width = 4) noexcept {
            for (std::size_t index = 0; index < width; ++index)
                bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
        }

        /** @brief Write a fixed-size RIFF chunk identifier without a terminator. */
        void Four(std::vector<std::byte> &bytes, const std::size_t offset, const char (&text)[5]) noexcept {
            for (std::size_t index = 0; index < 4; ++index)
                bytes[offset + index] = static_cast<std::byte>(text[index]);
        }

        /** @brief Encode the standard extensible IEEE-float header, including the non-PCM fact frame count. */
        void Header(std::vector<std::byte> &bytes, const WaveShape &shape, const std::uint32_t rate) noexcept {
            Four(bytes, 0, "RIFF");
            Integer(bytes, 4, static_cast<std::uint32_t>(shape.encodedBytes - 8));
            Four(bytes, 8, "WAVE");
            Four(bytes, 12, "fmt ");
            Integer(bytes, 16, 40);
            Integer(bytes, 20, 0xfffe, 2);
            Integer(bytes, 22, shape.count, 2);
            Integer(bytes, 24, rate);
            Integer(bytes, 28, rate * shape.count * 4);
            Integer(bytes, 32, shape.count * 4, 2);
            Integer(bytes, 34, 32, 2);
            Integer(bytes, 36, 22, 2);
            Integer(bytes, 38, 32, 2);
            Integer(bytes, 40, shape.mask);
            constexpr std::array<std::uint8_t, 16> FloatGuid{3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
            for (std::size_t index = 0; index < FloatGuid.size(); ++index)
                bytes[44 + index] = static_cast<std::byte>(FloatGuid[index]);
            Four(bytes, 60, "fact");
            Integer(bytes, 64, 4);
            Integer(bytes, 68, static_cast<std::uint32_t>(shape.frames));
            Four(bytes, 72, "data");
            Integer(bytes, 76, static_cast<std::uint32_t>(shape.encodedBytes - WaveHeaderBytes));
        }

    }  // namespace

    /** @copydoc EncodeWave */
    Result<std::vector<std::byte>> EncodeWave(const WaveShape &shape, const std::uint32_t sampleRate,
                                              const std::span<const Audio::AudioSample> samples, const WaveCancellation &cancellation) {
        using Encoded = Result<std::vector<std::byte>>;
        if (cancellation.IsRequested())
            return Encoded::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
        if (samples.size() != shape.frames * shape.count)
            return Encoded::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        std::vector<std::byte> bytes(static_cast<std::size_t>(shape.encodedBytes));
        Header(bytes, shape, sampleRate);
        for (std::uint64_t frame = 0; frame < shape.frames; ++frame) {
            if (frame % 4096 == 0 && cancellation.IsRequested()) {
                Security::SecureZero(bytes);
                return Encoded::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            }
            for (std::size_t channel = 0; channel < shape.count; ++channel) {
                const auto input = static_cast<std::size_t>(frame) * shape.count + shape.channels[channel].second;
                const auto output = WaveHeaderBytes + (static_cast<std::size_t>(frame) * shape.count + channel) * 4;
                Integer(bytes, static_cast<std::size_t>(output), std::bit_cast<std::uint32_t>(samples[input]));
            }
        }
        return Encoded::Success(std::move(bytes));
    }

}  // namespace Horo::Application::Internal
