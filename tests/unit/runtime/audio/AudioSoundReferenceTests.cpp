#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioSoundReference.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Audio {
    namespace {
        template <typename Identity> Identity IdentityValue(const std::uint8_t suffix) {
            if constexpr (std::is_same_v<Identity, AudioClipId> || std::is_same_v<Identity, AudioSoundId>) {
                std::array<std::uint8_t, 16> bytes{};
                bytes.back() = suffix;
                auto result = Identity::Create(Assets::AssetId::FromBytes(bytes));
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            } else {
                auto result = Identity::Create(suffix);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        }
    }  // namespace

    TEST_CASE("Audio sound references cover core definitions and extension contributions", "[unit][audio][sound-reference]") {
        const auto clip = AudioSoundReference::ForClip(IdentityValue<AudioClipId>(1));
        const auto variation = AudioSoundReference::ForVariation(IdentityValue<AudioSoundId>(2));
        const auto stream = AudioSoundReference::ForStream(IdentityValue<AudioSoundId>(3));
        const auto music = AudioSoundReference::ForMusic(IdentityValue<AudioSoundId>(4));
        const auto extension = AudioSoundReference::ForExtension(IdentityValue<AudioContributionId>(5), IdentityValue<AudioSoundId>(6));
        REQUIRE(clip.HasValue());
        REQUIRE(variation.HasValue());
        REQUIRE(stream.HasValue());
        REQUIRE(music.HasValue());
        REQUIRE(extension.HasValue());
        CHECK(ValidateAudioSoundReference(clip.Value()).HasValue());
        CHECK(ValidateAudioSoundReference(variation.Value()).HasValue());
        CHECK(ValidateAudioSoundReference(stream.Value()).HasValue());
        CHECK(ValidateAudioSoundReference(music.Value()).HasValue());
        CHECK(ValidateAudioSoundReference(extension.Value()).HasValue());
        CHECK(clip.Value().kind == AudioSoundReferenceKind::Clip);
        CHECK(variation.Value().kind == AudioSoundReferenceKind::Variation);
        CHECK(stream.Value().kind == AudioSoundReferenceKind::Stream);
        CHECK(music.Value().kind == AudioSoundReferenceKind::Music);
        CHECK(extension.Value().kind == AudioSoundReferenceKind::Extension);
        CHECK(std::holds_alternative<AudioClipId>(clip.Value().target));
        CHECK(std::holds_alternative<AudioSoundId>(variation.Value().target));
        CHECK(std::holds_alternative<AudioSoundId>(stream.Value().target));
        CHECK(std::holds_alternative<AudioSoundId>(music.Value().target));
        CHECK(std::holds_alternative<AudioSoundExtensionReference>(extension.Value().target));
        CHECK(std::get<AudioSoundId>(variation.Value().target) != std::get<AudioSoundId>(stream.Value().target));
        CHECK(std::get<AudioSoundExtensionReference>(extension.Value().target).contribution.Value() == 5);
    }

    TEST_CASE("Audio sound reference validation rejects mismatched or incomplete targets", "[unit][audio][sound-reference]") {
        AudioSoundReference reference{.kind = AudioSoundReferenceKind::Clip, .target = IdentityValue<AudioSoundId>(1)};
        auto result = ValidateAudioSoundReference(reference);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::SoundReferenceInvalid.code.Value());

        reference = {.kind = AudioSoundReferenceKind::Extension, .target = AudioSoundId{}};
        result = ValidateAudioSoundReference(reference);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::SoundReferenceInvalid.code.Value());

        auto invalid = AudioSoundReference::ForClip(AudioClipId{});
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == AudioErrors::SoundReferenceInvalid.code.Value());
    }

    TEST_CASE("Audio sound reference factories reject invalid identities", "[unit][audio][sound-reference]") {
        const auto validSound = IdentityValue<AudioSoundId>(7);
        const auto validContribution = IdentityValue<AudioContributionId>(8);
        const auto invalidCode = AudioErrors::SoundReferenceInvalid.code.Value();
        const auto requireInvalid = [&](const auto &result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == invalidCode);
        };

        requireInvalid(AudioSoundReference::ForClip(AudioClipId{}));
        requireInvalid(AudioSoundReference::ForVariation(AudioSoundId{}));
        requireInvalid(AudioSoundReference::ForStream(AudioSoundId{}));
        requireInvalid(AudioSoundReference::ForMusic(AudioSoundId{}));
        requireInvalid(AudioSoundReference::ForExtension(AudioContributionId{}, validSound));
        requireInvalid(AudioSoundReference::ForExtension(validContribution, AudioSoundId{}));
        requireInvalid(AudioSoundReference::ForExtension(validContribution, validSound, {0, 1}));
    }

    TEST_CASE("Audio sound definitions validate backend-neutral playback defaults", "[unit][audio][sound-reference]") {
        const auto source = AudioSoundReference::ForStream(IdentityValue<AudioSoundId>(1));
        REQUIRE(source.HasValue());
        AudioSoundDefinition definition{.source = source.Value()};
        REQUIRE(ValidateAudioSoundDefinition(definition).HasValue());
        CHECK(definition.playback.gain == 1.0F);
        CHECK(definition.playback.pitch == 1.0F);

        definition.playback.gain = -1.0F;
        REQUIRE(ValidateAudioSoundDefinition(definition).HasError());

        definition.playback = {.gain = 0.0F, .pitch = 8.0F, .bus = IdentityValue<AudioBusId>(2)};
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasValue());
        definition.playback.pitch = 0.0F;
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasError());
        definition.playback.pitch = 8.0001F;
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasError());
        definition.playback = {};
        definition.playback.gain = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasError());
        definition.playback = {};
        definition.playback.pitch = std::numeric_limits<float>::infinity();
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasError());
        definition.playback = {};
        definition.playback.bus = AudioBusId{};
        REQUIRE(ValidateAudioSoundPlaybackDefaults(definition.playback).HasError());
        definition.playback = {};
        definition.version = {2, 0};
        const auto versionResult = ValidateAudioSoundDefinition(definition);
        REQUIRE(versionResult.HasError());
        CHECK(versionResult.ErrorValue().code.Value() == AudioErrors::SoundDefinitionVersionUnsupported.code.Value());
    }
}  // namespace Horo::Audio
