#include "AllocationProbe.h"
#include "AudioVoicePlaybackTestFixture.h"
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioVoicePlayback.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        using enum AudioVoiceControl;
        using enum AudioResamplerQuality;

        using PlaybackTest::Control;
        using PlaybackTest::Owner;
        using PlaybackTest::Plan;
        using PlaybackTest::Playback;
        using PlaybackTest::Registry;
        using PlaybackTest::Samples;

        TEST_CASE("Resident voice controls render ramps and hold pause cursor", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Ready);
            CHECK(voice.Apply({voice.Voice(), Resume}) == &AudioErrors::VoiceInvalidTransition);
            Control(voice, Start);
            const auto initial = voice.Render(samples.Destination(8));
            REQUIRE(initial.error == nullptr);
            CHECK(initial.produced == 8);
            CHECK(samples.output[0] == Catch::Approx(0.25F));
            CHECK(samples.output[3] == 1.0F);
            CHECK(voice.Cursor().frame == 8);
            CHECK(voice.Apply({voice.Voice(), Start}) == &AudioErrors::VoiceInvalidTransition);
            Control(voice, Pause);
            CHECK(voice.Render(samples.Destination(2)).produced == 2);
            CHECK(samples.output[0] == 0.75F);
            CHECK(samples.output[1] == 0.5F);
            CHECK(voice.Cursor().frame == 8);
            CHECK(voice.Apply({voice.Voice(), Resume}) == &AudioErrors::VoiceInvalidTransition);
            CHECK(voice.Render(samples.Destination(8)).produced == 2);
            CHECK(samples.output[1] == 0.0F);
            CHECK(samples.output[2] == 0.0F);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Paused);
            Control(voice, Resume);
            CHECK(voice.Render(samples.Destination(4)).produced == 4);
            CHECK(samples.output[0] == 0.25F);
            CHECK(voice.Cursor().frame == 12);
        }

        TEST_CASE("Seek commits only after bounded ramp and discards prior PCM history", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            samples.pcm[100] = 8.0F;
            auto voice = Playback(registry, samples, 0.5);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(5)).produced == 5);
            CHECK(voice.Cursor().frame == 2);
            CHECK(voice.Cursor().fraction == 0.5);
            REQUIRE(voice.Apply({voice.Voice(), Seek, 100}) == nullptr);
            CHECK(voice.Cursor().frame == 2);
            CHECK(voice.Render(samples.Destination(0)).produced == 0);
            REQUIRE(voice.Render(samples.Destination(4)).error == nullptr);
            CHECK(voice.Cursor().frame == 100);
            CHECK(voice.Cursor().fraction == 0.0);
            REQUIRE(voice.Render(samples.Destination(1)).produced == 1);
            CHECK(samples.output[0] == 2.0F);
            CHECK(voice.Cursor().fraction == 0.5);
            CHECK(voice.Apply({voice.Voice(), Seek, 513}) == &AudioErrors::PlaybackRequestInvalid);
            CHECK(voice.Cursor().frame == 100);
        }

        TEST_CASE("Pitch swaps use prepared DSP and reject incompatible used or unsupported plans", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(8)).error == nullptr);
            auto doubled = std::move(AudioResampler::Create(Plan(2.0), 40'000'000).Value());
            REQUIRE(voice.SwapPitch(voice.Voice(), doubled) == nullptr);
            CHECK_FALSE(doubled.IsFresh());
            CHECK(voice.SwapPitch(voice.Voice(), doubled) == &AudioErrors::VoiceInvalidTransition);
            REQUIRE(voice.Render(samples.Destination(4)).error == nullptr);
            CHECK(voice.Cursor().frame == 8);
            REQUIRE(voice.Render(samples.Destination(4)).error == nullptr);
            CHECK(voice.Cursor().frame == 16);
            CHECK(voice.SwapPitch(voice.Voice(), doubled) == &AudioErrors::ResamplerInvalid);
            for (const double speed : {0.0, -1.0, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
                CHECK(voice.Apply({.voice = voice.Voice(), .control = SetPlaybackSpeed, .playbackSpeed = speed}) ==
                      &AudioErrors::OperationUnsupported);
            }
            CHECK(voice.Cursor().frame == 16);
            Control(voice, SetPlaybackSpeed);
            auto incompatible = std::move(AudioResampler::Create(Plan(1.0, Sinc32), 40'000'000).Value());
            CHECK(voice.SwapPitch(voice.Voice(), incompatible) == &AudioErrors::ResamplerInvalid);
        }

        TEST_CASE("Loop wraps fractional source time and remains partition invariant across kernels", "[audio][voice_playback]") {
            for (const auto quality : {Linear, Sinc32, Sinc64}) {
                auto firstRegistry = Registry();
                auto secondRegistry = Registry();
                Samples first;
                Samples second;
                for (std::size_t frame = 0; frame < first.pcm.size(); ++frame)
                    first.pcm[frame] = static_cast<float>(frame % 7);
                for (std::size_t frame = 0; frame < second.pcm.size(); ++frame)
                    second.pcm[frame] = first.pcm[frame];
                auto full = Playback(firstRegistry, first, 0.5, {true, 3, 7}, quality);
                auto split = Playback(secondRegistry, second, 0.5, {true, 3, 7}, quality);
                Control(full, Start);
                Control(split, Start);
                REQUIRE(full.Render(first.Destination(101)).produced == 101);
                std::vector<float> partitioned;
                for (const auto frames : {1U, 2U, 15U, 7U, 76U}) {
                    const auto result = split.Render(second.Destination(frames));
                    REQUIRE(result.error == nullptr);
                    REQUIRE(result.produced == frames);
                    partitioned.insert(partitioned.end(), second.output.begin(), second.output.begin() + frames);
                }
                CHECK(std::equal(partitioned.begin(), partitioned.end(), first.output.begin()));
                CHECK(full.Cursor().frame == split.Cursor().frame);
                CHECK(full.Cursor().frame == 6);
                CHECK(full.Cursor().fraction == 0.5);
                CHECK_FALSE(IsTerminalAudioVoiceState(firstRegistry.State(full.Voice()).Value()));
            }
        }

        TEST_CASE("Natural EOF drains bounded tails and terminal evidence is emitted once", "[audio][voice_playback]") {
            for (const auto quality : {Linear, Sinc32, Sinc64}) {
                auto registry = Registry();
                Samples samples;
                auto voice = Playback(registry, samples, 1.0, {}, quality, 8);
                Control(voice, Start);
                const auto result = voice.Render(samples.Destination(256));
                REQUIRE(result.error == nullptr);
                CHECK(result.produced >= 8);
                CHECK(result.produced <= 8 + Plan(1.0, quality).Taps());
                CHECK(result.terminal);
                CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Finished);
                CHECK(voice.Cursor().frame == 8);
                CHECK(samples.output[result.produced] == 0.0F);
                CHECK_FALSE(voice.Render(samples.Destination()).terminal);
                CHECK(voice.Apply({voice.Voice(), Start}) == &AudioErrors::VoiceInvalidTransition);
            }
        }

        TEST_CASE("Stop overrides partial discontinuity without restoring old amplitude", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(8)).error == nullptr);
            Control(voice, Pause);
            REQUIRE(voice.Render(samples.Destination(2)).error == nullptr);
            Control(voice, Stop);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Stopping);
            const auto stopped = voice.Render(samples.Destination(8));
            CHECK(stopped.produced == 4);
            CHECK(samples.output[0] == 0.375F);
            CHECK(stopped.terminal);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Stopped);
            CHECK_FALSE(voice.Render(samples.Destination()).terminal);
        }

        TEST_CASE("Ready paused empty and endpoint controls preserve state preconditions", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto ready = Playback(registry, samples);
            CHECK(ready.Apply({ready.Voice(), Pause}) == &AudioErrors::VoiceInvalidTransition);
            REQUIRE(ready.Apply({ready.Voice(), Seek, 512}) == nullptr);
            Control(ready, Start);
            CHECK(ready.Render(samples.Destination()).terminal);
            auto empty = Playback(registry, samples, 1.0, {}, Linear, 0);
            Control(empty, Start);
            CHECK(empty.Render(samples.Destination()).terminal);
            auto paused = Playback(registry, samples);
            Control(paused, Start);
            REQUIRE(paused.Render(samples.Destination()).error == nullptr);
            Control(paused, Pause);
            REQUIRE(paused.Render(samples.Destination()).error == nullptr);
            REQUIRE(paused.Apply({paused.Voice(), Seek, 100}) == nullptr);
            CHECK(paused.Cursor().frame == 100);
            Control(paused, Stop);
            CHECK(paused.Render(samples.Destination()).terminal);
        }

        TEST_CASE("Loop changes reset only after ramp and malformed controls are transactional", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(20)).error == nullptr);
            CHECK(voice.Apply({.voice = voice.Voice(), .control = SetLoop, .loop = {true, 8, 8}}) == &AudioErrors::PlaybackRequestInvalid);
            REQUIRE(voice.Apply({.voice = voice.Voice(), .control = SetLoop, .loop = {true, 3, 7}}) == nullptr);
            CHECK(voice.Cursor().frame == 20);
            REQUIRE(voice.Render(samples.Destination(4)).error == nullptr);
            CHECK(voice.Cursor().frame == 3);
            CHECK(voice.Apply({voice.Voice(), Seek, 7}) == &AudioErrors::PlaybackRequestInvalid);
            CHECK(voice.Apply({voice.Voice(), Resume, 99}) == &AudioErrors::PlaybackRequestInvalid);
            CHECK(voice.Apply({voice.Voice(), static_cast<AudioVoiceControl>(255)}) == &AudioErrors::PlaybackRequestInvalid);
        }

        TEST_CASE("Callback rejection shutdown cancellation and pitch swap allocate nothing", "[audio][voice_playback][realtime]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 0.5, {true, 0, 4}, Sinc64);
            auto candidate = std::move(AudioResampler::Create(Plan(2.0, Sinc64), 40'000'000).Value());
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto started = voice.Apply({voice.Voice(), Start});
            const auto rendered = voice.Render(samples.Destination());
            const auto rejected = voice.Apply({voice.Voice(), Start});
            const auto swapped = voice.SwapPitch(voice.Voice(), candidate);
            const auto changed = voice.Render(samples.Destination());
            const auto cancelled = voice.Apply({voice.Voice(), Cancel});
            const auto terminal = voice.Render(samples.Destination());
            const auto duplicate = voice.Apply({voice.Voice(), Cancel});
            const auto after = Horo::Tests::AllocationProbe::Count();
            CHECK(after == before);
            CHECK(started == nullptr);
            CHECK(rendered.error == nullptr);
            CHECK(rejected == &AudioErrors::VoiceInvalidTransition);
            CHECK(swapped == nullptr);
            CHECK(changed.error == nullptr);
            CHECK(cancelled == nullptr);
            CHECK(terminal.terminal);
            CHECK(duplicate == &AudioErrors::VoiceInvalidTransition);
            CHECK(registry.Snapshot(voice.Voice()).Value().terminalReason == AudioVoiceTerminalReason::Cancelled);
        }

        TEST_CASE("Prepared voice teardown and stale generations fence replacement", "[audio][voice_playback]") {
            auto registry = Registry(1);
            Samples samples;
            AudioVoiceHandle old;
            {
                auto voice = Playback(registry, samples);
                old = voice.Voice();
                auto moved = std::move(voice);
                CHECK(voice.Render(samples.Destination()).error == &AudioErrors::RuntimeInactive);
                CHECK_FALSE(voice.Voice().IsValid());
                CHECK(voice.Cursor().frame == 0);
                Control(moved, Start);
            }
            auto replacement = Playback(registry, samples);
            CHECK(replacement.Voice().generation == old.generation + 1);
            CHECK(replacement.Apply({old, Stop}) == &AudioErrors::HandleStale);
            auto foreign = replacement.Voice();
            foreign.owner = AudioRuntimeId::Create(554).Value();
            CHECK(replacement.Apply({foreign, Start}) == &AudioErrors::HandleOwnerMismatch);
            CHECK(replacement.Apply({{}, Start}) == &AudioErrors::HandleMalformed);
            REQUIRE(registry.BeginShutdown().HasValue());
            const auto rendered = replacement.Render(samples.Destination());
            CHECK(rendered.terminal);
            CHECK(rendered.produced == 0);
            CHECK(std::bit_cast<std::uint32_t>(samples.output[0]) == 0);
        }

        TEST_CASE("Malformed output and preparation failure do not consume PCM or registry slots", "[audio][voice_playback]") {
            auto registry = Registry(1);
            Samples samples;
            const AudioVoicePlaybackConfig invalid{Plan(), {}, 4, 1, 40'000'000};
            const AudioVoicePlaybackConfig valid{Plan(), {}, 4, 1'000'000, 40'000'000};
            CHECK(AudioVoicePlayback::Create(registry, samples.Source(), invalid).HasError());
            bool allocationFailed{};
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure;
                allocationFailed = AudioVoicePlayback::Create(registry, samples.Source(), valid).HasError();
            }
            CHECK(allocationFailed);
            auto voice = Playback(registry, samples);
            Control(voice, Start);
            std::array<std::span<float>, 1> misaligned{std::span{samples.output}.subspan(1)};
            CHECK(voice.Render({misaligned, 16}).error == &AudioErrors::ResamplerInvalid);
            CHECK(samples.output[0] == -9.0F);
            CHECK(voice.Cursor().frame == 0);
            CHECK(voice.Render(samples.Destination(257)).error == &AudioErrors::ResamplerInvalid);
            CHECK(voice.Cursor().frame == 0);
        }

        TEST_CASE("Resident PCM ownership and fade silence survive caller mutation", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            samples.pcm.fill(std::numeric_limits<float>::min());
            auto voice = Playback(registry, samples);
            samples.pcm.fill(9.0F);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(4)).produced == 4);
            CHECK(std::bit_cast<std::uint32_t>(samples.output[0]) == 0);
            CHECK(samples.output[3] == std::numeric_limits<float>::min());
            Control(voice, Stop);
            REQUIRE(voice.Render(samples.Destination(4)).terminal);
            for (const auto sample : std::span{samples.output}.first(4))
                CHECK(std::bit_cast<std::uint32_t>(sample) == 0);
        }

        TEST_CASE("Playback rejects overlapping channel output before writing and preserves channel order", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples left;
            Samples right;
            right.pcm.fill(-2.0F);
            const std::array<std::span<const float>, 2> input{left.pcm, right.pcm};
            const auto plan = AudioResamplerPlan::Prepare({.quality = Linear,
                                                           .inputRate = 48000,
                                                           .outputRate = 48000,
                                                           .channels = 2,
                                                           .maximumOutputFrames = 256},
                                                          {1'000'000, 1'000'000, 4096})
                                  .Value();
            auto voice = std::move(AudioVoicePlayback::Create(registry, {input, 512, false}, {plan, {}, 1, 1'000'000, 40'000'000}).Value());
            Control(voice, Start);
            const std::array<std::span<float>, 2> aliases{left.output, left.output};
            CHECK(voice.Render({aliases, 8}).error == &AudioErrors::ResamplerInvalid);
            CHECK(left.output[0] == -9.0F);
            CHECK(voice.Cursor().frame == 0);
            const std::array<std::span<float>, 2> output{left.output, right.output};
            REQUIRE(voice.Render({output, 8}).produced == 8);
            CHECK(left.output[0] == 1.0F);
            CHECK(right.output[0] == -2.0F);
            CHECK(voice.Render({std::span{output}.first(1), 8}).error == &AudioErrors::ResamplerInvalid);
        }

        TEST_CASE("Prepared pitch before start and short loops support multiple source wraps", "[audio][voice_playback]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 1.0, {true, 0, 1});
            auto candidate = std::move(AudioResampler::Create(Plan(8.0), 40'000'000).Value());
            REQUIRE(voice.SwapPitch(voice.Voice(), candidate) == nullptr);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(16)).produced == 16);
            CHECK(voice.Cursor().frame == 0);
            CHECK(voice.Cursor().fraction == 0.0);
            CHECK(samples.output[15] == 1.0F);
            const AudioCommand stop{{Owner(), 1, {Owner(), 1, 1}}, AudioVoiceControlRequest{voice.Voice(), Stop}};
            CHECK(ClassifyAudioCommand(stop) == AudioCommandClass::Critical);
            AudioCommand normalized;
            CHECK(NormalizeAudioCommand(stop, normalized) == AudioCommandStatus::Ok);
        }

        struct PlaybackPort final {
            AudioVoicePlayback &voice;
            AudioCommandBuffer &commands;
            std::uint32_t terminalCount{};
            float firstSample{};

            bool Consume(const Backend::RenderInvocation &invocation) noexcept {
                AudioCommandRecord record;
                for (std::uint32_t count = 0; count < 4 && commands.TryConsume(record); ++count) {
                    const auto *control = std::get_if<AudioVoiceControlRequest>(&record.command.payload);
                    if (!control || record.command.scope.epoch != invocation.epoch.callbackEpoch || voice.Apply(*control))
                        return false;
                }
                return true;
            }

            static Backend::RenderResult Process(void *context, const Backend::RenderInvocation &invocation) noexcept {
                using enum Backend::RenderDisposition;
                auto &port = *static_cast<PlaybackPort *>(context);
                if (invocation.phase != Backend::RenderPhase::Rendering)
                    return PlaybackTest::SilentPhase(invocation);
                if (!port.Consume(invocation))
                    return {};
                std::array<std::span<float>, 1> planes{std::span{invocation.output.planes[0], invocation.output.capacityFrames}};
                const auto result = port.voice.Render({planes, invocation.output.validFrames});
                port.terminalCount += result.terminal ? 1U : 0U;
                port.firstSample = planes[0][0];
                return result.error ? Backend::RenderResult{} : Backend::RenderResult{Rendered, AudioCallbackFaultCode::None};
            }
        };

        TEST_CASE("Actual Null callbacks consume voice controls through the production SPSC buffer", "[audio][voice_playback][null]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 1.0, {true, 0, 512});
            auto commands =
                std::move(AudioCommandBuffer::Create({Owner(), AudioMemoryPoolId::Create(553).Value(), 1, 4, 1, 16384}).Value());
            PlaybackPort port{voice, commands};
            auto backend = std::move(Backend::CreateNullAudioBackend({Owner(), 1, 1, 1}).Value());
            const AudioDeviceEpoch epoch{{Owner(), 1, 1}, 1, 1};
            PlaybackTest::Start(*backend, epoch, {&port, PlaybackPort::Process});
            std::uint64_t sequence{};
            const auto publish = [&](const AudioVoiceControl control) {
                const AudioCommandRecord record{++sequence,
                                                {{Owner(), 1, {Owner(), 1, 1}}, AudioVoiceControlRequest{voice.Voice(), control}}};
                REQUIRE(commands.TryPublish(record) == AudioCommandPublishStatus::Published);
            };
            publish(Start);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(voice.Cursor().frame == 128);
            CHECK(port.firstSample == 0.25F);
            publish(Pause);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(voice.Cursor().frame == 128);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Paused);
            publish(Resume);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(voice.Cursor().frame == 256);
            publish(Stop);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.terminalCount == 1);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.terminalCount == 1);
            CHECK(commands.Depth() == 0);
            PlaybackTest::Shutdown(*backend, commands, epoch);
        }
    }  // namespace
}  // namespace Horo::Audio
