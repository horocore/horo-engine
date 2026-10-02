#include "AllocationProbe.h"
#include "Horo/Audio/CoreAudioDSPNode.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Audio {
    namespace {
        constexpr std::uint32_t Frames = 256;
        constexpr double SampleTolerance = 2e-6;
        constexpr double ResponseTolerance = 2e-4;

        struct Fixture final {
            CoreAudioDSPNode node;
            alignas(64) std::array<std::byte, 1024> state{};
            alignas(64) std::array<float, MaximumAudioChannels * Frames> input{};
            alignas(64) std::array<float, MaximumAudioChannels * Frames> output{};
            std::array<float *, MaximumAudioChannels> inputPlanes{};
            std::array<float *, MaximumAudioChannels> outputPlanes{};
            std::array<AudioDSPPortBuffer, 1> inputs{};
            std::array<AudioDSPPortBuffer, 1> outputs{};
            std::array<AudioDSPParameterValue, 1> parameters{};
            AudioDSPProcessContext process;

            explicit Fixture(CoreAudioDSPKind kind, AudioChannelLayout layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono))
                : node(kind, {48'000, std::move(layout)}, Frames) {
                REQUIRE(node.Prepare({Frames, state, {}}).HasValue());
                for (std::size_t channel = 0; channel < MaximumAudioChannels; ++channel) {
                    inputPlanes[channel] = input.data() + channel * Frames;
                    outputPlanes[channel] = output.data() + channel * Frames;
                }
                const auto &descriptor = node.Descriptor();
                inputs[0] = {.connected = true, .block = Block(descriptor.inputs[0].format, inputPlanes, Frames)};
                outputs[0] = {.connected = true, .block = Block(descriptor.outputs[0].format, outputPlanes, 0)};
                parameters[0] = {descriptor.parameters[0].identity, descriptor.parameters[0].defaultValue,
                                 descriptor.parameters[0].defaultValue, 0};
                process = {.inputs = inputs, .outputs = outputs, .parameters = parameters, .stateStorage = state, .frames = Frames};
            }

            static AudioPlanarBlockView Block(const AudioProcessingFormat &format, const std::array<float *, MaximumAudioChannels> &planes,
                                              const std::uint32_t valid) {
                return {ViewAudioChannelLayout(format.layout), format.sampleRate,
                        std::span<float *const>{planes.data(), format.layout.orderedChannels.size()}, valid, Frames};
            }

            void Value(const float value) {
                parameters[0].value = parameters[0].target = value;
            }

            void Run() {
                REQUIRE(node.Process(process).status == AudioDSPProcessStatus::Processed);
            }
        };

        TEST_CASE("Core gain preserves headroom and explicit sample-boundary smoothing", "[audio][dsp][core]") {
            Fixture fixture(CoreAudioDSPKind::Gain);
            std::fill(fixture.input.begin(), fixture.input.end(), 1.0F);
            fixture.parameters[0] = {fixture.parameters[0].identity, 0.0F, 4.0F, 8};
            fixture.Run();
            for (std::size_t frame = 0; frame < Frames; ++frame)
                REQUIRE(fixture.output[frame] == Catch::Approx(4.0 * std::min(frame + 1, std::size_t{8}) / 8).margin(SampleTolerance));
            fixture.Value(0.0F);
            fixture.Run();
            REQUIRE_FALSE(std::signbit(fixture.output[0]));
        }

        TEST_CASE("Pan energy and stereo balance follow declared speaker semantics", "[audio][dsp][core]") {
            Fixture mono(CoreAudioDSPKind::Pan);
            std::fill(mono.input.begin(), mono.input.end(), 1.0F);
            for (const float pan : {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F}) {
                mono.Value(pan);
                mono.Run();
                const double left = mono.output[0];
                const double right = mono.output[Frames];
                REQUIRE(left * left + right * right == Catch::Approx(1.0).margin(SampleTolerance));
                REQUIRE(left == Catch::Approx(std::cos((pan + 1) * std::numbers::pi / 4)).margin(SampleTolerance));
                REQUIRE(right == Catch::Approx(std::sin((pan + 1) * std::numbers::pi / 4)).margin(SampleTolerance));
            }
            Fixture stereo(CoreAudioDSPKind::Pan, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo));
            std::fill(stereo.input.begin(), stereo.input.end(), 0.5F);
            stereo.Run();
            REQUIRE(stereo.output[0] == Catch::Approx(0.5).margin(SampleTolerance));
            REQUIRE(stereo.output[Frames] == Catch::Approx(0.5).margin(SampleTolerance));
            mono.Value(0.0F);
            mono.outputPlanes[0] = mono.input.data();
            mono.Run();
            REQUIRE(mono.input[0] == Catch::Approx(std::numbers::sqrt2 / 2).margin(SampleTolerance));
            REQUIRE(mono.output[Frames] == Catch::Approx(std::numbers::sqrt2 / 2).margin(SampleTolerance));
        }

        TEST_CASE("Filter impulses match analytic poles and reset removes prior history", "[audio][dsp][core]") {
            const double pole = std::exp(-2.0 * std::numbers::pi * 1000.0 / 48000.0);
            for (const auto kind : {CoreAudioDSPKind::LowPass, CoreAudioDSPKind::HighPass}) {
                Fixture fixture(kind);
                fixture.input[0] = 1.0F;
                fixture.Run();
                for (std::size_t frame = 0; frame < Frames; ++frame) {
                    const double low = (1.0 - pole) * std::pow(pole, frame);
                    const double expected = kind == CoreAudioDSPKind::LowPass ? low : (frame == 0 ? 1.0 : 0.0) - low;
                    REQUIRE(fixture.output[frame] == Catch::Approx(expected).margin(SampleTolerance));
                }
                fixture.node.Reset();
                fixture.Run();
                REQUIRE(fixture.output[0] == Catch::Approx(kind == CoreAudioDSPKind::LowPass ? 1.0 - pole : pole).margin(SampleTolerance));
                std::fill(fixture.input.begin(), fixture.input.end(), 1.0F);
                fixture.node.Reset();
                fixture.Run();
                REQUIRE(fixture.output[Frames - 1] == Catch::Approx(kind == CoreAudioDSPKind::LowPass ? 1.0 : 0.0).margin(SampleTolerance));
            }
        }

        TEST_CASE("Log-frequency sweep fixtures match analytic steady-state filter response", "[audio][dsp][core]") {
            const double pole = std::exp(-2.0 * std::numbers::pi * 1000.0 / 48000.0);
            for (const auto kind : {CoreAudioDSPKind::LowPass, CoreAudioDSPKind::HighPass}) {
                Fixture fixture(kind);
                for (const double frequency : {187.5, 750.0, 3000.0, 6000.0, 12000.0}) {
                    fixture.node.Reset();
                    for (std::size_t frame = 0; frame < Frames; ++frame)
                        fixture.input[frame] = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * frame / 48000.0));
                    fixture.Run();  // settle history, then measure a complete integer-period block
                    fixture.Run();
                    double energy = 0.0;
                    for (std::size_t frame = 0; frame < Frames; ++frame)
                        energy += static_cast<double>(fixture.output[frame]) * fixture.output[frame];
                    const double cosine = std::cos(2.0 * std::numbers::pi * frequency / 48000.0);
                    const double denominator = 1.0 + pole * pole - 2.0 * pole * cosine;
                    const double numerator =
                        kind == CoreAudioDSPKind::LowPass ? (1.0 - pole) * (1.0 - pole) : 2.0 * pole * pole * (1.0 - cosine);
                    REQUIRE(std::sqrt(2.0 * energy / Frames) ==
                            Catch::Approx(std::sqrt(numerator / denominator)).margin(ResponseTolerance));
                }
            }
        }

        TEST_CASE("Explicit smoothing segments and filter history are invariant to block partition", "[audio][dsp][core]") {
            for (const auto kind : {CoreAudioDSPKind::Gain, CoreAudioDSPKind::Pan, CoreAudioDSPKind::LowPass, CoreAudioDSPKind::HighPass}) {
                Fixture whole(kind);
                Fixture split(kind);
                std::fill(whole.input.begin(), whole.input.end(), 1.0F);
                std::fill(split.input.begin(), split.input.end(), 1.0F);
                const float start = kind == CoreAudioDSPKind::Pan ? -1.0F : 1.0F;
                const float target = kind == CoreAudioDSPKind::Pan ? 1.0F : kind == CoreAudioDSPKind::Gain ? 4.0F : 2000.0F;
                whole.parameters[0] = {whole.parameters[0].identity, start, target, Frames};
                whole.Run();
                split.parameters[0] = {split.parameters[0].identity, start, target, Frames};
                split.process.frames = Frames / 2;
                split.Run();
                std::array<float, Frames> firstLeft{};
                std::copy_n(split.output.begin(), Frames / 2, firstLeft.begin());
                split.parameters[0].value = (start + target) / 2.0F;
                split.parameters[0].rampFrames = Frames / 2;
                split.Run();
                for (std::size_t frame = 0; frame < Frames / 2; ++frame) {
                    REQUIRE(firstLeft[frame] == Catch::Approx(whole.output[frame]).margin(SampleTolerance));
                    REQUIRE(split.output[frame] == Catch::Approx(whole.output[frame + Frames / 2]).margin(SampleTolerance));
                }
            }
        }

        TEST_CASE("Bypass freezes history and preparation bounds remain authoritative", "[audio][dsp][core]") {
            Fixture fixture(CoreAudioDSPKind::LowPass);
            fixture.process.frames = 1;
            fixture.input[0] = 1.0F;
            fixture.Run();
            fixture.input[0] = 0.0F;
            fixture.process.bypass = AudioDSPBypassMode::CopyMainInput;
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Bypassed);
            REQUIRE(fixture.output[0] == 0.0F);
            fixture.process.bypass = AudioDSPBypassMode::Silence;
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Bypassed);
            REQUIRE(fixture.output[0] == 0.0F);
            fixture.process.bypass = AudioDSPBypassMode::Process;
            fixture.Run();
            const double pole = std::exp(-2.0 * std::numbers::pi * 1000.0 / 48000.0);
            REQUIRE(fixture.output[0] == Catch::Approx(pole * (1.0 - pole)).margin(SampleTolerance));
            REQUIRE(fixture.node.Prepare({16, fixture.state, {}}).HasValue());
            fixture.Run();
            REQUIRE(fixture.output[0] == 0.0F);
            REQUIRE(fixture.node.Prepare({0, fixture.state, {}}).HasError());
            fixture.process.frames = 17;
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
            fixture.process.frames = 1;
            fixture.Run();
            CoreAudioDSPNode unknown(static_cast<CoreAudioDSPKind>(255), {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)});
            unknown.Reset();
            REQUIRE(unknown.Prepare({16, fixture.state, {}}).HasError());
            REQUIRE(unknown.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
        }

        std::vector<AudioChannelLayout> Layouts() {
            std::vector<AudioChannelLayout> layouts;
            for (const auto preset :
                 {AudioSpeakerPreset::Mono, AudioSpeakerPreset::Stereo, AudioSpeakerPreset::TwoPointOne, AudioSpeakerPreset::Quad,
                  AudioSpeakerPreset::FivePointOne, AudioSpeakerPreset::SevenPointOne, AudioSpeakerPreset::SevenPointOneFour})
                layouts.push_back(MakeAudioSpeakerLayout(preset));
            AudioChannelLayout discrete{AudioLayoutKind::Discrete, {}, {}};
            for (std::uint8_t channel = 0; channel < MaximumAudioChannels; ++channel)
                discrete.orderedChannels.push_back(AudioDiscreteChannel{channel});
            layouts.push_back(discrete);
            for (std::uint8_t order = 0; order <= 3; ++order) {
                AudioChannelLayout ambisonic{AudioLayoutKind::Ambisonic, {}, AmbisonicDescriptor{order}};
                for (std::uint8_t channel = 0; channel < (order + 1) * (order + 1); ++channel)
                    ambisonic.orderedChannels.push_back(AudioAmbisonicChannel{channel});
                layouts.push_back(ambisonic);
            }
            return layouts;
        }

        TEST_CASE("Gain and filter admission preserves every layout family", "[audio][dsp][core]") {
            const auto layouts = Layouts();
            for (const auto &layout : layouts) {
                for (const auto kind : {CoreAudioDSPKind::Gain, CoreAudioDSPKind::LowPass, CoreAudioDSPKind::HighPass}) {
                    Fixture fixture(kind, layout);
                    fixture.process.frames = 16;
                    for (std::size_t channel = 0; channel < layout.orderedChannels.size(); ++channel)
                        fixture.input[channel * Frames] = static_cast<float>(channel + 1);
                    fixture.Run();
                    for (std::size_t channel = 0; channel < layout.orderedChannels.size(); ++channel)
                        REQUIRE(fixture.output[channel * Frames] / (channel + 1) ==
                                Catch::Approx(fixture.output[0]).margin(SampleTolerance));
                    const float expected = fixture.output[0];
                    fixture.node.Reset();
                    std::copy(fixture.inputPlanes.begin(), fixture.inputPlanes.end(), fixture.outputPlanes.begin());
                    fixture.Run();
                    REQUIRE(fixture.input[0] == Catch::Approx(expected).margin(SampleTolerance));
                }
                if (layout != MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono) &&
                    layout != MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)) {
                    CoreAudioDSPNode pan(CoreAudioDSPKind::Pan, {48'000, layout});
                    REQUIRE(ValidateAudioDSPNodeDescriptor(pan.Descriptor()).HasError());
                }
            }
        }

        TEST_CASE("Filter tail accounting is bounded and bypass preserves the remaining budget", "[audio][dsp][core]") {
            Fixture fixture(CoreAudioDSPKind::LowPass);
            fixture.process.frames = 1;
            fixture.input[0] = 1.0F;
            const auto active = fixture.node.Process(fixture.process);
            REQUIRE(active.remainingTailFrames == fixture.node.Descriptor().tailFrames);
            fixture.input[0] = 0.0F;
            fixture.process.bypass = AudioDSPBypassMode::Silence;
            REQUIRE(fixture.node.Process(fixture.process).remainingTailFrames == active.remainingTailFrames);
            fixture.process.bypass = AudioDSPBypassMode::Process;
            REQUIRE(fixture.node.Process(fixture.process).remainingTailFrames == active.remainingTailFrames - 1);
            std::uint32_t remaining = active.remainingTailFrames - 1;
            bool bounded = true;
            while (remaining != 0) {
                fixture.process.frames = std::min(remaining, Frames);
                const auto tail = fixture.node.Process(fixture.process);
                bounded = bounded && tail.processedFrames == fixture.process.frames;
                remaining = tail.remainingTailFrames;
            }
            REQUIRE(bounded);
            fixture.process.frames = 1;
            REQUIRE(fixture.node.Process(fixture.process).remainingTailFrames == 0);
            REQUIRE(fixture.output[0] == 0.0F);
            fixture.node.Reset();
            REQUIRE(fixture.node.Process(fixture.process).remainingTailFrames == 0);
            REQUIRE(fixture.output[0] == 0.0F);
        }

        TEST_CASE("Core callbacks allocate nothing and reject sample storage aliasing state", "[audio][dsp][core]") {
            for (const auto kind : {CoreAudioDSPKind::Gain, CoreAudioDSPKind::Pan, CoreAudioDSPKind::LowPass, CoreAudioDSPKind::HighPass}) {
                Fixture fixture(kind);
                const auto before = Tests::AllocationProbe::Count();
                const auto result = fixture.node.Process(fixture.process);
                fixture.node.Reset();
                const auto after = Tests::AllocationProbe::Count();
                REQUIRE(result.status == AudioDSPProcessStatus::Processed);
                REQUIRE(after == before);
                fixture.inputPlanes[0] = reinterpret_cast<float *>(fixture.state.data());
                REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
                fixture.inputPlanes[0] = fixture.input.data();
                fixture.outputPlanes[0] = reinterpret_cast<float *>(fixture.state.data());
                REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
            }
        }

        TEST_CASE("Core nodes reject stale storage, malformed parameters and partial overlap transactionally", "[audio][dsp][core]") {
            Fixture fixture(CoreAudioDSPKind::Gain);
            fixture.output[0] = 7.0F;
            fixture.parameters[0].target = std::numeric_limits<float>::infinity();
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
            REQUIRE(fixture.output[0] == 7.0F);
            fixture.Value(1.0F);
            fixture.outputPlanes[0] = fixture.input.data() + 16;
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
            fixture.outputPlanes[0] = fixture.output.data();
            alignas(64) std::array<std::byte, 1024> other{};
            fixture.process.stateStorage = other;
            REQUIRE(fixture.node.Process(fixture.process).status == AudioDSPProcessStatus::Rejected);
            fixture.process.stateStorage = fixture.state;
            fixture.input[0] = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(fixture.node.Process(fixture.process).fault == AudioDSPFault::InvalidInput);
            REQUIRE(fixture.output[0] == 0.0F);
            fixture.input[0] = std::numeric_limits<float>::max();
            fixture.Value(16.0F);
            REQUIRE(fixture.node.Process(fixture.process).fault == AudioDSPFault::NonFiniteOutput);
            REQUIRE(fixture.output[0] == 0.0F);
            fixture.input[0] = std::numeric_limits<float>::denorm_min();
            fixture.Value(1.0F);
            fixture.Run();
            REQUIRE(fixture.output[0] == 0.0F);
            REQUIRE_FALSE(std::signbit(fixture.output[0]));
        }
    }  // namespace
}  // namespace Horo::Audio
