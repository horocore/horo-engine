#include "AllocationProbe.h"
#include "AudioVoicePlaybackTestFixture.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/ScheduledAudioCommandBatch.h"

#include <bit>
#include <vector>

namespace Horo::Audio {
    namespace {
        using namespace PlaybackTest;
        using enum AudioVoiceControl;
        using enum AudioResamplerQuality;

        /** @brief Exercise one owned voice without assertions or allocation inside the measured processing interval. */
        bool ChurnVoice(AudioVoicePlayback &voice, Samples &samples, const bool physical, std::uint32_t &terminals) {
            bool valid = true;
            const auto render = [&](const std::uint32_t frames) {
                const auto result = voice.Render(samples.Destination(frames));
                valid = valid && result.error == nullptr && !result.terminal;
            };
            const auto control = [&](const AudioVoiceControl operation) {
                const auto *error = voice.Apply({voice.Voice(), operation});
                valid = valid && error == nullptr;
            };
            render(13);
            render(27);
            valid = valid && voice.Cursor().frame == 4 && voice.Cursor().fraction == 0.0;
            control(physical ? Virtualize : Realize);
            render(9);
            valid = valid && voice.Cursor().frame == 4 && voice.Cursor().fraction == 0.5;
            control(Pause);
            render(16);
            valid = valid && voice.Cursor().frame == 4 && voice.Cursor().fraction == 0.5;
            control(Resume);
            render(3);
            valid = valid && voice.Cursor().frame == 6 && voice.Cursor().fraction == 0.0;
            control(Cancel);
            const auto terminal = voice.Render(samples.Destination(1));
            terminals += terminal.terminal ? 1U : 0U;
            valid = valid && terminal.error == nullptr;
            const auto repeated = voice.Render(samples.Destination(1));
            valid = valid && repeated.error == nullptr && !repeated.terminal;
            return valid;
        }

        TEST_CASE("Full resident registries preserve loop cursors and terminal once through physical virtual churn",
                  "[audio][virtualization][qualification]") {
            for (const std::uint32_t capacity : {1U, 64U, 256U, 1024U}) {
                CAPTURE(capacity);
                auto registry = Registry(capacity);
                Samples samples;
                std::vector<AudioVoicePlayback> voices;
                voices.reserve(capacity);
                for (std::uint32_t index = 0; index < capacity; ++index) {
                    voices.push_back(Playback(registry, samples, 0.5, {true, 3, 7}));
                    Control(voices.back(), index % 2 == 0 ? Start : StartVirtual);
                }
                REQUIRE(registry.CreateVoice().HasError());
                bool valid = true;
                std::uint32_t terminals{};
                const auto allocations = Horo::Tests::AllocationProbe::Count();
                const auto frees = Horo::Tests::AllocationProbe::FreeCount();
                for (std::uint32_t index = 0; index < capacity; ++index) {
                    auto &voice = voices[index];
                    valid = ChurnVoice(voice, samples, index % 2 == 0, terminals) && valid;
                }
                const auto allocationEnd = Horo::Tests::AllocationProbe::Count();
                const auto freeEnd = Horo::Tests::AllocationProbe::FreeCount();
                CHECK(valid);
                CHECK(terminals == capacity);
                CHECK(allocationEnd == allocations);
                CHECK(freeEnd == frees);
                for (const auto &voice : voices)
                    CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Cancelled);
                voices.clear();
                const auto replacement = registry.CreateVoice();
                REQUIRE(replacement.HasValue());
                REQUIRE(registry.Cancel(replacement.Value()).HasValue());
                REQUIRE(registry.Release(replacement.Value()).HasValue());
            }
        }

        TEST_CASE("Scheduled virtual start retains equal-time control order and starts source time at the target",
                  "[audio][virtualization][batch]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 0.5);
            const AudioCommandScope scope{Owner(), 1, {Owner(), 1, 1}};
            ScheduledAudioCommandBatch batch{.target = {AudioCommandTargetKind::ExactSampleFrame, 5, 1, 1}, .commandCount = 2};
            batch.commands[0] = {scope, AudioVoiceControlRequest{voice.Voice(), StartVirtual}};
            batch.commands[1] = {scope, AudioVoiceControlRequest{.voice = voice.Voice(), .control = SetLoop, .loop = {true, 1, 4}}};
            const AudioMemoryHandle storage{Owner(), AudioMemoryPoolId::Create(553).Value(), 1, 1};
            AudioCommand command;
            REQUIRE(MakeScheduledAudioBatchCommand(batch, storage, command) == ScheduledAudioCommandBatchStatus::Ok);
            auto commands = std::move(AudioCommandBuffer::Create({Owner(), storage.pool, 1, 4, 1, 16384}).Value());
            REQUIRE(commands.TryPublish({1, command}) == AudioCommandPublishStatus::Published);
            AudioCommandRecord record;
            REQUIRE(commands.TryConsume(record));
            const auto &target = std::get<AudioScheduledBatchCommand>(record.command.payload);
            REQUIRE(target.target.sampleFrame == 5);
            // Host dispatcher splits the block at the retained batch target, not at producer wall time.
            REQUIRE(voice.Render(samples.Destination(static_cast<std::uint32_t>(target.target.sampleFrame))).error == nullptr);
            CHECK(voice.Cursor().frame == 0);
            for (std::uint32_t index = 0; index < target.commandCount; ++index)
                REQUIRE(voice.Apply(std::get<AudioVoiceControlRequest>(batch.commands[index].payload)) == nullptr);
            REQUIRE(voice.Render(samples.Destination(7)).error == nullptr);
            CHECK(voice.Cursor().frame == 3);
            CHECK(voice.Cursor().fraction == 0.5);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Virtual);
        }

        TEST_CASE("Virtual resident time preserves loop phase across realization and block partitions", "[audio][virtualization]") {
            auto fullRegistry = Registry();
            auto splitRegistry = Registry();
            Samples fullSamples;
            Samples splitSamples;
            auto full = Playback(fullRegistry, fullSamples, 0.5, {true, 3, 7});
            auto split = Playback(splitRegistry, splitSamples, 0.5, {true, 3, 7});
            Control(full, StartVirtual);
            Control(split, StartVirtual);
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto bulk = full.Render(fullSamples.Destination(101));
            bool splitSucceeded = true;
            for (const auto frames : {1U, 2U, 15U, 7U, 76U})
                splitSucceeded = splitSucceeded && split.Render(splitSamples.Destination(frames)).error == nullptr;
            const auto after = Horo::Tests::AllocationProbe::Count();
            CHECK(after == before);
            CHECK(splitSucceeded);
            CHECK(bulk.error == nullptr);
            CHECK(bulk.produced == 0);
            CHECK(full.Cursor().frame == 6);
            CHECK(full.Cursor().fraction == 0.5);
            CHECK(split.Cursor().frame == full.Cursor().frame);
            CHECK(split.Cursor().fraction == full.Cursor().fraction);
            for (const auto sample : std::span{fullSamples.output}.first(101))
                CHECK(std::bit_cast<std::uint32_t>(sample) == 0);
            Control(full, Realize);
            CHECK(full.Cursor().frame == 6);
            CHECK(full.Cursor().fraction == 0.5);
            REQUIRE(full.Render(fullSamples.Destination(4)).error == nullptr);
            CHECK(full.Cursor().frame == 4);
            CHECK(full.Cursor().fraction == 0.5);
            CHECK(fullSamples.output[3] == 1.0F);
        }

        TEST_CASE("Virtual pause controls EOF and cancellation retain canonical terminal evidence", "[audio][virtualization]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 8.0, {}, Linear, 8);
            Control(voice, StartVirtual);
            Control(voice, Pause);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Paused);
            CHECK(voice.Render(samples.Destination(32)).produced == 0);
            CHECK(voice.Cursor().frame == 0);
            Control(voice, Resume);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Virtual);
            CHECK(voice.Render(samples.Destination(0)).terminal == false);
            CHECK(voice.Render(samples.Destination(1)).terminal);
            CHECK(voice.Cursor().frame == 8);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Finished);
            CHECK_FALSE(voice.Render(samples.Destination(32)).terminal);
            CHECK(voice.Apply({voice.Voice(), Realize}) == &AudioErrors::VoiceInvalidTransition);
            auto cancelled = Playback(registry, samples);
            Control(cancelled, StartVirtual);
            Control(cancelled, Cancel);
            CHECK(cancelled.Render(samples.Destination(1)).terminal);
            CHECK_FALSE(cancelled.Render(samples.Destination(1)).terminal);
        }

        TEST_CASE("Virtualization preserves audible rather than decoder lookahead cursor", "[audio][virtualization]") {
            auto registry = Registry();
            Samples samples;
            auto voice = Playback(registry, samples, 0.5, {true, 2, 9}, Sinc64);
            Control(voice, Start);
            REQUIRE(voice.Render(samples.Destination(5)).error == nullptr);
            Control(voice, Virtualize);
            CHECK(voice.Cursor().frame == 2);
            CHECK(voice.Cursor().fraction == 0.5);
            REQUIRE(voice.Render(samples.Destination(3)).error == nullptr);
            CHECK(voice.Cursor().frame == 4);
            REQUIRE(voice.Apply({voice.Voice(), Seek, 6}) == nullptr);
            CHECK(voice.Cursor().frame == 6);
            CHECK(voice.Cursor().fraction == 0.0);
            Control(voice, Stop);
            CHECK(voice.Render(samples.Destination(1)).terminal);
            CHECK(registry.State(voice.Voice()).Value() == AudioVoiceState::Stopped);
        }

    }  // namespace
}  // namespace Horo::Audio
