#pragma once

/** @file AudioRepeatedPlaybackTestFixture.h
 * @brief Shared resident playback fixtures and exact replay assertions.
 */
#include "AllocationProbe.h"
#include "AudioPlaybackTestSupport.h"
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/AudioRepeatedPlayback.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

namespace Horo::Audio::RepeatedPlaybackTest {
    inline AudioRuntimeId Runtime() {
        return AudioRuntimeId::Create(557).Value();
    }

    inline AudioClipId Clip(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return AudioClipId::Create(Assets::AssetId::FromBytes(bytes)).Value();
    }

    inline AudioSoundId Sound() {
        return AudioSoundId::Create(Clip(99).Asset()).Value();
    }

    inline AudioRepeatedPlaybackConfig Config(const std::uint32_t voices = 4) {
        return {Runtime(),
                1,
                1,
                1,
                8,
                voices,
                {.quality = AudioResamplerQuality::Linear,
                 .inputRate = 48000,
                 .outputRate = 48000,
                 .channels = 1,
                 .maximumOutputFrames = 256},
                {1'000'000, 1'000'000, 4096},
                4,
                1'000'000,
                40'000'000};
    }

    inline AudioRepeatedPlayback Owner(const std::uint32_t voices = 4) {
        auto prepared = AudioRepeatedPlayback::Create(Config(voices));
        REQUIRE(prepared.HasValue());
        return std::move(prepared).Value();
    }

    inline AudioPlaybackBinding Binding(const bool variation = false, const std::uint32_t emitter = 1, const std::uint32_t owner = 1,
                                        const std::uint32_t scene = 1) {
        AudioPlaybackSettings settings;
        settings.spatialMode = AudioSpatialMode::TwoD;
        const auto sound = variation ? AudioSoundReference::ForVariation(Sound()).Value() : AudioSoundReference::ForClip(Clip(1)).Value();
        return {MakeAudioPlaybackRequest({Runtime(), scene, 1}, sound, settings).Value(),
                {Runtime(), emitter, 1},
                {Runtime(), owner, 1},
                1};
    }

    inline AudioVariationAssetSchema Variation(const AudioVariationSelection selection) {
        return {CurrentAudioAssetSchemaVersion,
                selection,
                {{Clip(3), selection == AudioVariationSelection::WeightedRandom ? 4.0F : 1.0F},
                 {Clip(1), 1.0F},
                 {Clip(2), selection == AudioVariationSelection::WeightedRandom ? 2.0F : 1.0F}},
                {-2, 2},
                {-6, 0},
                42};
    }

    struct Samples final : PlaybackTest::SampleBuffers {
        std::array<AudioResolvedPlaybackClip, 3> clips;

        Samples() {
            for (std::uint32_t index = 0; index < clips.size(); ++index)
                clips[index] = {Clip(static_cast<std::uint8_t>(index + 1)), {inputs, 512, true}, 48000};
        }

        std::span<const AudioResolvedPlaybackClip> Resolved(const bool variation) const {
            return variation ? std::span{clips} : std::span{clips}.first(1);
        }

        AudioResamplerOutput Output(const std::uint32_t count = 16) {
            return Destination(count);
        }
    };

    inline AudioRepeatedPlaybackRequest Request(const AudioPlaybackLaneHandle lane, const AudioPlaybackBinding &binding,
                                                const std::uint64_t sequence) {
        return {lane, sequence, binding.prototype, {}};
    }

    inline AudioRepeatedPlaybackReceipt Submit(AudioRepeatedPlayback &owner, const AudioRepeatedPlaybackRequest &request,
                                               const Samples &samples, const bool variation = false, const std::uint64_t frame = 0) {
        auto result = owner.Submit(request, samples.Resolved(variation), {1, frame});
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline void Apply(AudioRepeatedPlayback &owner, const AudioRepeatedPlaybackReceipt &receipt, const std::uint64_t frame = 0) {
        REQUIRE(receipt.commands.commandCount == 1);
        REQUIRE(owner.Apply(receipt.commands.commands[0], {1, frame}) == nullptr);
    }

    inline void Retire(AudioRepeatedPlayback &owner, const AudioRepeatedPlaybackReceipt &receipt) {
        REQUIRE(owner.Cancel(receipt.voice).HasValue());
        REQUIRE(owner.Release(receipt.voice).HasValue());
    }

    inline void SameChoice(const AudioRepeatedPlaybackReceipt &a, const AudioRepeatedPlaybackReceipt &b) {
        CHECK(a.clip == b.clip);
        CHECK(std::bit_cast<std::uint32_t>(a.pitchDeltaSemitones) == std::bit_cast<std::uint32_t>(b.pitchDeltaSemitones));
        CHECK(std::bit_cast<std::uint32_t>(a.gainDeltaDb) == std::bit_cast<std::uint32_t>(b.gainDeltaDb));
        CHECK(a.pitch == Catch::Approx(b.pitch).epsilon(2e-6));
        CHECK(a.gain == Catch::Approx(b.gain).epsilon(2e-6));
    }

}  // namespace Horo::Audio::RepeatedPlaybackTest
