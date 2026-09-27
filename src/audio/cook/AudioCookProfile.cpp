#include "Horo/Audio/AudioCookProfile.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Horo::Audio {
    namespace {
        constexpr std::size_t MaximumTargetOverrides = 64;
        constexpr std::size_t MaximumToolchainIdentityBytes = 128;

        [[nodiscard]] bool ValidResidency(const AudioCookResidency residency) noexcept {
            using enum AudioCookResidency;
            switch (residency) {
                case Auto:
                case Resident:
                case Streamed:
                    return true;
            }
            return false;
        }

        void Append32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void Append64(std::vector<std::uint8_t> &bytes, const std::uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void AppendText(std::vector<std::uint8_t> &bytes, const std::string_view text) {
            Append32(bytes, static_cast<std::uint32_t>(text.size()));
            bytes.insert(bytes.end(), text.begin(), text.end());
        }

        void AppendLayout(std::vector<std::uint8_t> &bytes, const std::optional<AudioChannelLayout> &layout) {
            bytes.push_back(layout.has_value() ? 1 : 0);
            if (!layout.has_value())
                return;
            bytes.push_back(static_cast<std::uint8_t>(layout->kind));
            bytes.push_back(static_cast<std::uint8_t>(layout->orderedChannels.size()));
            bytes.push_back(layout->ambisonic.has_value() ? 1 : 0);
            bytes.push_back(layout->ambisonic.has_value() ? layout->ambisonic->order : 0);
            for (const auto &channel : layout->orderedChannels) {
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

        Result<void> ValidateSettings(const AudioCookSettings &settings) {
            if (!settings.container.IsValid() || !settings.codec.IsValid() || settings.streamThresholdFrames == 0 ||
                settings.streamChunkFrames == 0 || settings.streamChunkFrames > MaximumAudioStreamChunkFrames ||
                (settings.sampleRate.has_value() &&
                 (*settings.sampleRate < MinimumAudioSampleRate || *settings.sampleRate > MaximumAudioSampleRate)) ||
                (settings.layout.has_value() && !ValidateAudioChannelLayout(ViewAudioChannelLayout(*settings.layout))) ||
                !ValidResidency(settings.residency))
                return Result<void>::Failure(MakeError(AudioErrors::CookProfileInvalid));
            if (settings.container != AudioContainerIds::HoroCooked || settings.codec != AudioCodecIds::Pcm ||
                settings.compression != AudioCookCompression::None || settings.quality != AudioCookQuality::Float32Exact ||
                settings.encoderDelayFrames != 0 || settings.encoderPaddingFrames != 0)
                return Result<void>::Failure(MakeError(AudioErrors::CookCombinationUnsupported));
            return Result<void>::Success();
        }

        Result<const AudioCookSettings *> SelectSettings(const AudioCookProfile &profile, const AssetCookTargetId &target,
                                                         bool &overridden) {
            if (!target.IsValid() || profile.overrides.size() > MaximumTargetOverrides)
                return Result<const AudioCookSettings *>::Failure(MakeError(AudioErrors::CookProfileInvalid));
            if (auto valid = ValidateSettings(profile.defaults); valid.HasError())
                return Result<const AudioCookSettings *>::Failure(valid.ErrorValue());
            const AudioCookSettings *selected = &profile.defaults;
            for (std::size_t index = 0; index < profile.overrides.size(); ++index) {
                const auto &candidate = profile.overrides[index];
                if (!candidate.target.IsValid())
                    return Result<const AudioCookSettings *>::Failure(MakeError(AudioErrors::CookProfileInvalid));
                if (auto valid = ValidateSettings(candidate.settings); valid.HasError())
                    return Result<const AudioCookSettings *>::Failure(valid.ErrorValue());
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (profile.overrides[previous].target == candidate.target)
                        return Result<const AudioCookSettings *>::Failure(MakeError(AudioErrors::CookProfileInvalid));
                }
                if (candidate.target == target) {
                    selected = &candidate.settings;
                    overridden = true;
                }
            }
            return Result<const AudioCookSettings *>::Success(selected);
        }
    }  // namespace

    /** @copydoc FingerprintAudioCookConfiguration */
    Result<Sha256Digest> FingerprintAudioCookConfiguration(const AudioCookProfile &profile, const AssetCookTargetId &target,
                                                           const AudioCookToolchain &toolchain) {
        if (toolchain.identity.empty() || toolchain.identity.size() > MaximumToolchainIdentityBytes ||
            std::ranges::any_of(toolchain.identity, [](const unsigned char c) {
            return c < 0x21 || c > 0x7E;
        }))
            return Result<Sha256Digest>::Failure(MakeError(AudioErrors::CookProfileInvalid));
        bool overridden = false;
        auto selected = SelectSettings(profile, target, overridden);
        if (selected.HasError())
            return Result<Sha256Digest>::Failure(selected.ErrorValue());
        const auto &settings = *selected.Value();
        std::vector<std::uint8_t> bytes;
        AppendText(bytes, "horo.audio.cook.configuration.v1");
        Append32(bytes, AudioCookSchemaVersion);
        AppendText(bytes, target.Value());
        AppendText(bytes, toolchain.identity);
        bytes.push_back(overridden ? 1 : 0);
        Append32(bytes, settings.container.value);
        Append32(bytes, settings.codec.value);
        bytes.push_back(static_cast<std::uint8_t>(settings.compression));
        bytes.push_back(static_cast<std::uint8_t>(settings.quality));
        bytes.push_back(static_cast<std::uint8_t>(settings.residency));
        bytes.push_back(settings.sampleRate.has_value() ? 1 : 0);
        Append32(bytes, settings.sampleRate.value_or(0));
        AppendLayout(bytes, settings.layout);
        Append64(bytes, settings.streamThresholdFrames);
        Append32(bytes, settings.streamChunkFrames);
        Append32(bytes, settings.encoderDelayFrames);
        Append32(bytes, settings.encoderPaddingFrames);
        return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{bytes})));
    }

    /** @copydoc ResolveAudioCookPlan */
    Result<AudioCookPlan> ResolveAudioCookPlan(const AudioSourceImportCandidate &source, const AudioCookProfile &profile,
                                               const AssetCookTargetId &target, const AudioCookToolchain &toolchain) {
        auto fingerprint = FingerprintAudioCookConfiguration(profile, target, toolchain);
        if (fingerprint.HasError())
            return Result<AudioCookPlan>::Failure(fingerprint.ErrorValue());
        bool overridden = false;
        auto selected = SelectSettings(profile, target, overridden);
        if (selected.HasError())
            return Result<AudioCookPlan>::Failure(selected.ErrorValue());
        if (source.frameCount == 0 || !source.container.IsValid() || !source.codec.IsValid() || source.decoderIdentity.empty() ||
            source.decoderIdentity.size() > MaximumToolchainIdentityBytes || !ValidateAudioProcessingFormat(source.decodedFormat))
            return Result<AudioCookPlan>::Failure(MakeError(AudioErrors::SourceInvalid));
        const auto &settings = *selected.Value();
        if ((settings.sampleRate.has_value() && *settings.sampleRate != source.decodedFormat.sampleRate) ||
            (settings.layout.has_value() && *settings.layout != source.decodedFormat.layout))
            return Result<AudioCookPlan>::Failure(MakeError(AudioErrors::CookCombinationUnsupported));
        AudioCookResidency residency = settings.residency;
        if (residency == AudioCookResidency::Auto)
            residency = source.frameCount >= settings.streamThresholdFrames ? AudioCookResidency::Streamed : AudioCookResidency::Resident;
        return Result<AudioCookPlan>::Success(AudioCookPlan{
            .target = target,
            .settings = settings,
            .sourceContainer = source.container,
            .sourceCodec = source.codec,
            .decoderIdentity = source.decoderIdentity,
            .outputFormat = source.decodedFormat,
            .residency = residency,
            .frameCount = source.frameCount,
            .targetOverride = overridden,
            .configurationDigest = fingerprint.Value(),
        });
    }
}  // namespace Horo::Audio
