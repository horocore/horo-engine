#pragma once

#include "AudioPlaybackTestSupport.h"
#include "Horo/Audio/AudioVoicePlayback.h"

namespace Horo::Audio::PlaybackTest {
    /** @brief Stable test-owned registry domain shared by physical and virtual playback regressions. */
    inline AudioRuntimeId Owner() {
        return AudioRuntimeId::Create(553).Value();
    }

    inline AudioVoiceStateMachine Registry(const std::uint32_t capacity = 4) {
        return std::move(AudioVoiceStateMachine::Create({.owner = Owner(), .maximumVoices = capacity}).Value());
    }

    inline AudioResamplerPlan Plan(const double pitch = 1.0, const AudioResamplerQuality quality = AudioResamplerQuality::Linear) {
        auto plan = AudioResamplerPlan::Prepare({.quality = quality,
                                                 .inputRate = 48000,
                                                 .outputRate = 48000,
                                                 .channels = 1,
                                                 .maximumOutputFrames = 256,
                                                 .pitch = pitch},
                                                {1'000'000, 1'000'000, 4096});
        REQUIRE(plan.HasValue());
        return plan.Value();
    }

    using Samples = SampleBuffers;

    /** @brief Prepare identical owned source/conversion reservations without bypassing production admission. */
    inline AudioVoicePlayback Playback(AudioVoiceStateMachine &registry, const Samples &samples, const double pitch = 1.0,
                                       const AudioVoiceLoop loop = {}, const AudioResamplerQuality quality = AudioResamplerQuality::Linear,
                                       const std::uint32_t sourceFrames = 512) {
        auto voice =
            AudioVoicePlayback::Create(registry, samples.Source(sourceFrames), {Plan(pitch, quality), loop, 4, 1'000'000, 40'000'000});
        REQUIRE(voice.HasValue());
        return std::move(voice).Value();
    }

    inline void Control(AudioVoicePlayback &playback, const AudioVoiceControl control) {
        REQUIRE(playback.Apply({playback.Voice(), control}) == nullptr);
    }
}  // namespace Horo::Audio::PlaybackTest
