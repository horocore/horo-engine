#include "Horo/Audio/AudioDSPNode.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>

namespace Horo::Audio {
    namespace {
        AudioDSPNodeDescriptor Descriptor() {
            const auto format = AudioProcessingFormat{48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)};
            return {.inputs = {{.kind = AudioDSPPortKind::Main, .format = format, .maximumFrames = 128},
                               {.kind = AudioDSPPortKind::Sidechain, .format = format, .maximumFrames = 128, .optional = true}},
                    .outputs = {{.kind = AudioDSPPortKind::Main, .format = format, .maximumFrames = 128}},
                    .parameters =
                        {{.identity = AudioParameterId::Create(11).Value(), .minimum = 0.0F, .maximum = 2.0F, .defaultValue = 1.0F}},
                    .memory = {.stateBytes = 64, .scratchBytes = 128},
                    .maximumFrames = 128,
                    .latencyFrames = 32,
                    .tailFrames = 256,
                    .supportsBypass = true,
                    .supportsReset = true,
                    .inPlace = false,
                    .allocationFree = true,
                    .blockingFree = true,
                    .loggingFree = true,
                    .externalCallbackFree = true};
        }

        struct StereoBlock final {
            alignas(64) std::array<AudioSample, 256> storage{};
            std::array<AudioSample *, 2> planes{storage.data(), storage.data() + 128};

            AudioPlanarBlockView View(const AudioProcessingFormat &format, const std::uint32_t validFrames = 64) {
                return {.layout = ViewAudioChannelLayout(format.layout),
                        .sampleRate = format.sampleRate,
                        .planes = planes,
                        .validFrames = validFrames,
                        .capacityFrames = 128};
            }
        };

        TEST_CASE("Audio DSP descriptor admits complete bounded multi-I/O requirements", "[unit][audio][dsp]") {
            const auto descriptor = Descriptor();
            REQUIRE(ValidateAudioDSPNodeDescriptor(descriptor).HasValue());

            alignas(64) std::array<std::byte, 64> state{};
            alignas(64) std::array<std::byte, 128> scratch{};
            REQUIRE(ValidateAudioDSPPreparation(descriptor, {.maximumFrames = 128, .stateStorage = state, .scratchStorage = scratch})
                        .HasValue());
        }

        TEST_CASE("Audio DSP descriptor rejects incomplete memory and real-time guarantees", "[unit][audio][dsp]") {
            auto descriptor = Descriptor();
            descriptor.memory.stateBytes = MaximumAudioDSPStateBytes + 1;
            REQUIRE(ValidateAudioDSPNodeDescriptor(descriptor).HasError());

            descriptor = Descriptor();
            descriptor.blockingFree = false;
            REQUIRE(ValidateAudioDSPNodeDescriptor(descriptor).HasError());

            descriptor = Descriptor();
            alignas(64) std::array<std::byte, 63> state{};
            alignas(64) std::array<std::byte, 128> scratch{};
            REQUIRE(ValidateAudioDSPPreparation(descriptor, {.maximumFrames = 128, .stateStorage = state, .scratchStorage = scratch})
                        .HasError());
        }

        TEST_CASE("Audio DSP process validates sidechains, parameters, frames and bypass", "[unit][audio][dsp]") {
            const auto descriptor = Descriptor();
            const auto format = descriptor.inputs.front().format;
            StereoBlock input;
            StereoBlock output;
            alignas(64) std::array<std::byte, 64> state{};
            alignas(64) std::array<std::byte, 128> scratch{};
            const std::array<AudioDSPPortBuffer, 2> inputs{{{.connected = true, .block = input.View(format)}, {.connected = false}}};
            std::array<AudioDSPPortBuffer, 1> outputs{{{.connected = true, .block = output.View(format, 0)}}};
            std::array<AudioDSPParameterValue, 1> parameters{
                {{.identity = AudioParameterId::Create(11).Value(), .value = 1.0F, .target = 1.5F, .rampFrames = 32}}};
            auto process = AudioDSPProcessContext{.inputs = inputs,
                                                  .outputs = outputs,
                                                  .parameters = parameters,
                                                  .stateStorage = state,
                                                  .scratchStorage = scratch,
                                                  .frames = 64,
                                                  .bypass = AudioDSPBypassMode::CopyMainInput};
            REQUIRE(ValidateAudioDSPProcess(descriptor, process));

            process.frames = 129;
            REQUIRE_FALSE(ValidateAudioDSPProcess(descriptor, process));
            process.frames = 64;
            parameters[0].identity = AudioParameterId::Create(12).Value();
            REQUIRE_FALSE(ValidateAudioDSPProcess(descriptor, process));

            parameters[0].identity = AudioParameterId::Create(11).Value();
            process.stateStorage = std::span<std::byte>{state}.subspan(1, 64);
            REQUIRE_FALSE(ValidateAudioDSPProcess(descriptor, process));
        }

        TEST_CASE("Audio DSP process rejects a required disconnected port and invalid output shape", "[unit][audio][dsp]") {
            const auto descriptor = Descriptor();
            const auto format = descriptor.inputs.front().format;
            StereoBlock input;
            StereoBlock output;
            alignas(64) std::array<std::byte, 64> state{};
            alignas(64) std::array<std::byte, 128> scratch{};
            std::array<AudioDSPPortBuffer, 2> inputs{{{.connected = false}, {.connected = false}}};
            std::array<AudioDSPPortBuffer, 1> outputs{{{.connected = true, .block = output.View(format, 0)}}};
            const std::array<AudioDSPParameterValue, 1> parameters{
                {{.identity = AudioParameterId::Create(11).Value(), .value = 1.0F, .target = 1.0F}}};
            auto process = AudioDSPProcessContext{.inputs = inputs,
                                                  .outputs = outputs,
                                                  .parameters = parameters,
                                                  .stateStorage = state,
                                                  .scratchStorage = scratch,
                                                  .frames = 64};
            REQUIRE_FALSE(ValidateAudioDSPProcess(descriptor, process));
            inputs[0] = {.connected = true, .block = input.View(format, 32)};
            REQUIRE_FALSE(ValidateAudioDSPProcess(descriptor, process));
        }
    }  // namespace
}  // namespace Horo::Audio
