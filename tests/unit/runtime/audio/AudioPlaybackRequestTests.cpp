#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioPlaybackRequest.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>

namespace Horo::Audio {
    namespace {
        template <typename Identity> Identity IdentityValue(const std::uint8_t suffix) {
            if constexpr (std::is_same_v<Identity, AudioClipId> || std::is_same_v<Identity, AudioSoundId>) {
                std::array<std::uint8_t, 16> bytes{};
                bytes.back() = suffix;
                return Identity::Create(Assets::AssetId::FromBytes(bytes)).Value();
            } else {
                return Identity::Create(suffix).Value();
            }
        }

        AudioSceneContextHandle SceneContext() {
            return {.owner = IdentityValue<AudioRuntimeId>(1), .slot = 2, .generation = 3};
        }

        AudioSoundReference Sound() {
            return AudioSoundReference::ForClip(IdentityValue<AudioClipId>(4)).Value();
        }
    }  // namespace

    static_assert(!std::is_same_v<decltype(std::declval<AudioPlaybackRequest>().playback), AudioVoiceHandle>);

    TEST_CASE("Audio playback settings validate admission and spatial policy", "[unit][audio][playback-request]") {
        AudioPlaybackSettings settings;
        REQUIRE(ValidateAudioSoundPlaybackDefaults(settings).HasValue());
        CHECK(settings.spatialMode == AudioSpatialMode::ThreeD);
        CHECK(settings.priority == 128);

        settings.spatialMode = AudioSpatialMode::TwoD;
        CHECK(ValidateAudioSoundPlaybackDefaults(settings).HasValue());
        settings.enableDoppler = true;
        CHECK(ValidateAudioSoundPlaybackDefaults(settings).HasValue());

        settings = {};
        settings.priority = static_cast<AudioPriority>(MaximumAudioPriority + 1);
        CHECK(ValidateAudioSoundPlaybackDefaults(settings).HasError());

        settings = {};
        settings.concurrency.group = AudioConcurrencyGroupId{};
        CHECK(ValidateAudioSoundPlaybackDefaults(settings).HasError());

        settings = {};
        settings.concurrency.mode = static_cast<AudioConcurrencyMode>(255);
        CHECK(ValidateAudioSoundPlaybackDefaults(settings).HasError());
    }

    TEST_CASE("Two-dimensional playback request is provider independent", "[unit][audio][playback-request]") {
        AudioPlaybackSettings settings{.spatialMode = AudioSpatialMode::TwoD};
        const AudioPlaybackRequest request{.sceneContext = SceneContext(), .sound = Sound(), .playback = settings};

        REQUIRE(ValidateAudioPlaybackRequest(request).HasValue());
        CHECK_FALSE(request.RequiresSpatialProvider());
        CHECK(request.IsSceneBound());
        CHECK(request.sceneLifecycle == AudioSceneLifecyclePolicy::StopOnUnload);
    }

    TEST_CASE("Playback request requires assigned sound and live scene context", "[unit][audio][playback-request]") {
        AudioPlaybackRequest request{.sceneContext = SceneContext(), .sound = {}};
        auto result = ValidateAudioPlaybackRequest(request);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::PlaybackRequestInvalid.code.Value());

        request.sound = Sound();
        request.sceneContext = {};
        result = ValidateAudioPlaybackRequest(request);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::PlaybackRequestInvalid.code.Value());
    }

    TEST_CASE("Playback request preserves typed playback validation errors", "[unit][audio][playback-request]") {
        AudioPlaybackRequest request{.sceneContext = SceneContext(), .sound = Sound()};
        request.playback.priority = static_cast<AudioPriority>(MaximumAudioPriority + 1);
        auto result = ValidateAudioPlaybackRequest(request);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::SoundDefinitionInvalid.code.Value());

        request.playback = {};
        request.sceneLifecycle = static_cast<AudioSceneLifecyclePolicy>(255);
        result = ValidateAudioPlaybackRequest(request);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioErrors::SoundDefinitionInvalid.code.Value());
    }

    TEST_CASE("Playback request factory keeps lifecycle policy in the transient value", "[unit][audio][playback-request]") {
        AudioPlaybackSettings settings{.loop = true, .priority = 240};
        const auto request = MakeAudioPlaybackRequest(SceneContext(), Sound(), settings, AudioSceneLifecyclePolicy::KeepAliveInHostContext);
        REQUIRE(request.HasValue());
        CHECK(request.Value().playback == settings);
        CHECK(request.Value().sceneLifecycle == AudioSceneLifecyclePolicy::KeepAliveInHostContext);
        CHECK(request.Value().RequiresSpatialProvider());
    }
}  // namespace Horo::Audio
