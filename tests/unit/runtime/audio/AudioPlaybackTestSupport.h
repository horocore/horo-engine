#pragma once

/** @file AudioPlaybackTestSupport.h
 * @brief Shared resident PCM storage and actual Null lifecycle for playback regressions.
 */
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/AudioResampler.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Audio::PlaybackTest {
    /** @brief Nonmoving planar storage keeps every borrowed span bound to its own samples. */
    struct SampleBuffers {
        alignas(64) std::array<float, 512> pcm{};
        alignas(64) std::array<float, 256> output{};
        std::array<std::span<const float>, 1> inputs{pcm};
        std::array<std::span<float>, 1> outputs{output};

        SampleBuffers() {
            pcm.fill(1.0F);
            output.fill(-9.0F);
        }

        SampleBuffers(const SampleBuffers &) = delete;
        SampleBuffers &operator=(const SampleBuffers &) = delete;

        AudioResamplerInput Source(const std::uint32_t frames = 512) const {
            return {inputs, frames, true};
        }

        AudioResamplerOutput Destination(const std::uint32_t frames = 16) {
            return {outputs, frames};
        }
    };

    /** @brief Prime or quiesce with silence rather than consuming pending playback controls. */
    inline Backend::RenderResult SilentPhase(const Backend::RenderInvocation &invocation) noexcept {
        for (auto *plane : invocation.output.planes)
            std::fill_n(plane, invocation.output.validFrames, 0.0F);
        return {invocation.phase == Backend::RenderPhase::Priming ? Backend::RenderDisposition::Ready
                                                                  : Backend::RenderDisposition::Quiesced,
                AudioCallbackFaultCode::None};
    }

    /** @brief Complete one actual control operation and acknowledge its retained evidence. */
    inline void Complete(Backend::NullAudioBackend &backend, const Backend::Request &request) {
        const auto operation = backend.Begin(request, {1, 1'000'000'000});
        REQUIRE(operation.HasValue());
        REQUIRE(backend.AdvanceControl().HasValue());
        REQUIRE(backend.Poll(operation.Value()).Value().has_value());
        REQUIRE(backend.AcknowledgeCompletion(operation.Value()).HasValue());
    }

    /** @brief Start the real callback only after enumeration, opening and priming have succeeded. */
    inline void Start(Backend::NullAudioBackend &backend, const AudioDeviceEpoch epoch, const Backend::RenderPort port) {
        Complete(backend, Backend::Enumerate{});
        Complete(backend,
                 Backend::Open{epoch, {epoch.device, {48000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)}, {}, {128, 128, 128}}});
        Complete(backend, Backend::Start{epoch, port});
        REQUIRE(backend.AdvanceCallback().HasValue());
        REQUIRE(backend.CommitRendering(epoch).HasValue());
    }

    /** @brief Close command publication and prove callback quiescence before device teardown. */
    inline void Shutdown(Backend::NullAudioBackend &backend, AudioCommandBuffer &commands, const AudioDeviceEpoch epoch) {
        commands.Close();
        const auto quiesce = backend.Begin(Backend::Quiesce{epoch}, {1, 1'000'000'000}).Value();
        REQUIRE(backend.AdvanceControl().HasValue());
        REQUIRE(backend.AdvanceCallback().HasValue());
        REQUIRE(backend.Poll(quiesce).Value().has_value());
        REQUIRE(backend.AcknowledgeCompletion(quiesce).HasValue());
        Complete(backend, Backend::Stop{epoch});
        Complete(backend, Backend::Close{});
        CHECK(commands.IsDrained());
    }
}  // namespace Horo::Audio::PlaybackTest
