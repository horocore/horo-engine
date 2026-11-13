#include "AudioRepeatedPlaybackTestFixture.h"

namespace Horo::Audio {
    namespace {
        using namespace RepeatedPlaybackTest;

        TEST_CASE("Recorded seed sequence and canonical authored permutations replay real admissions", "[audio][repeated_playback]") {
            for (const auto selection : {AudioVariationSelection::Random, AudioVariationSelection::Shuffle,
                                         AudioVariationSelection::WeightedRandom, AudioVariationSelection::RoundRobin}) {
                auto first = Owner(1);
                auto second = Owner(1);
                const auto binding = Binding(true);
                auto variation = Variation(selection);
                const auto a = first.Bind(binding, {.variation = variation}).Value();
                if (selection != AudioVariationSelection::RoundRobin)
                    std::ranges::reverse(variation.entries);
                const auto b = second.Bind(binding, {.variation = variation}).Value();
                Samples samples;
                std::optional<AudioClipId> previous;
                for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
                    const auto left = Submit(first, Request(a, binding, sequence), samples, true);
                    const auto right = Submit(second, Request(b, binding, sequence), samples, true);
                    SameChoice(left, right);
                    CHECK(left.pitchDeltaSemitones >= -2);
                    CHECK(left.pitchDeltaSemitones <= 2);
                    CHECK(left.gainDeltaDb >= -6);
                    CHECK(left.gainDeltaDb <= 0);
                    CHECK(left.pitch == Catch::Approx(std::exp2(left.pitchDeltaSemitones / 12.0)).epsilon(2e-6));
                    CHECK(left.gain == Catch::Approx(std::pow(10.0, left.gainDeltaDb / 20.0)).epsilon(2e-6));
                    if (previous)
                        CHECK(left.clip != *previous);
                    previous = left.clip;
                    Apply(first, left);
                    REQUIRE(first.Render(left.voice, samples.Output()).error == nullptr);
                    CHECK(samples.output[3] == Catch::Approx(left.gain).epsilon(2e-6));
                    Retire(first, left);
                    Retire(second, right);
                }
            }
        }

        TEST_CASE("Shuffle bags exhaust once each with no boundary repeat", "[audio][repeated_playback]") {
            auto owner = Owner(1);
            const auto binding = Binding(true);
            const auto lane = owner.Bind(binding, {.variation = Variation(AudioVariationSelection::Shuffle)}).Value();
            Samples samples;
            std::optional<AudioClipId> previous;
            for (std::uint64_t bag = 0; bag < 100; ++bag) {
                std::array<AudioClipId, 3> choices;
                for (std::uint64_t index = 0; index < choices.size(); ++index) {
                    const auto receipt = Submit(owner, Request(lane, binding, bag * 3 + index + 1), samples, true);
                    choices[index] = receipt.clip;
                    if (previous)
                        CHECK(receipt.clip != *previous);
                    previous = receipt.clip;
                    Retire(owner, receipt);
                }
                CHECK(choices[0] != choices[1]);
                CHECK(choices[0] != choices[2]);
                CHECK(choices[1] != choices[2]);
            }
        }

        TEST_CASE("Rejected ignored pre-cancelled and deduplicated requests do not consume candidate random state",
                  "[audio][repeated_playback]") {
            auto owner = Owner(1);
            auto replay = Owner(1);
            auto binding = Binding(true);
            const auto policy = AudioRepeatedPlaybackPolicy{.variation = Variation(AudioVariationSelection::Shuffle)};
            const auto lane = owner.Bind(binding, policy).Value();
            const auto replayLane = replay.Bind(binding, policy).Value();
            Samples samples;
            const auto first = Submit(owner, Request(lane, binding, 1), samples, true);
            auto cached = Submit(owner, Request(lane, binding, 1), samples, true);
            CHECK(cached.disposition == AudioRepeatedPlaybackDisposition::Replay);
            CHECK(cached.commands.commandCount == 0);
            SameChoice(first, cached);
            CHECK(owner.Submit(Request(lane, binding, 2), samples.Resolved(true), {1, 0}).HasError());  // Real slot exhaustion.
            Retire(owner, first);
            auto cancelled = Request(lane, binding, 2);
            cancelled.cancelled = true;
            CHECK(Submit(owner, cancelled, samples, true).disposition == AudioRepeatedPlaybackDisposition::Cancelled);
            const auto next = Submit(owner, Request(lane, binding, 3), samples, true);
            const auto replayFirst = Submit(replay, Request(replayLane, binding, 1), samples, true);
            SameChoice(first, replayFirst);
            Retire(replay, replayFirst);
            SameChoice(next, Submit(replay, Request(replayLane, binding, 3), samples, true));
            auto conflict = Request(lane, binding, 3);
            conflict.playback.playback.gain = 0.5F;
            CHECK(owner.Submit(conflict, samples.Resolved(true), {1, 0}).HasError());
            CHECK(owner.Submit(Request(lane, binding, 2), samples.Resolved(true), {1, 0}).HasError());

            auto ignore = Owner(2);
            const auto ignoreLane = ignore.Bind(binding, {AudioRetriggerPolicy::Ignore, {}, policy.variation}).Value();
            const auto live = Submit(ignore, Request(ignoreLane, binding, 1), samples, true);
            CHECK(Submit(ignore, Request(ignoreLane, binding, 2), samples, true).disposition == AudioRepeatedPlaybackDisposition::Ignored);
            Retire(ignore, live);
            SameChoice(next, Submit(ignore, Request(ignoreLane, binding, 3), samples, true));
        }

        TEST_CASE("Restart targets exact live identity retains adjustments and rejects replayed old commands",
                  "[audio][repeated_playback]") {
            auto owner = Owner(1);
            const auto binding = Binding(true);
            const auto lane = owner.Bind(binding, {AudioRetriggerPolicy::Restart, {}, Variation(AudioVariationSelection::Random)}).Value();
            Samples samples;
            const auto first = Submit(owner, Request(lane, binding, 1), samples, true);
            CHECK(owner.Submit(Request(lane, binding, 2), samples.Resolved(true), {1, 0}).HasError());
            Apply(owner, first);
            REQUIRE(owner.Render(first.voice, samples.Output()).produced == 16);
            const auto restart = Submit(owner, Request(lane, binding, 2), samples, true);
            CHECK(restart.disposition == AudioRepeatedPlaybackDisposition::Restarted);
            CHECK(restart.voice == first.voice);
            SameChoice(first, restart);
            Apply(owner, restart);
            REQUIRE(owner.Render(first.voice, samples.Output(4)).error == nullptr);
            REQUIRE(owner.Render(first.voice, samples.Output(4)).error == nullptr);
            CHECK(samples.output[3] == Catch::Approx(first.gain).epsilon(2e-6));
            const auto newer = Submit(owner, Request(lane, binding, 3), samples, true);
            CHECK(owner.Apply(restart.commands.commands[0], {1, 0}) == &AudioErrors::PlaybackRequestInvalid);
            Apply(owner, newer);
            Retire(owner, newer);
            const auto replacement = Submit(owner, Request(lane, binding, 4), samples, true);
            CHECK(replacement.voice.slot == first.voice.slot);
            CHECK(replacement.voice.generation != first.voice.generation);
            CHECK(replacement.disposition == AudioRepeatedPlaybackDisposition::Admitted);
            CHECK(owner.Apply(first.commands.commands[0], {1, 0}) == &AudioErrors::HandleStale);
            CHECK(owner.Render(first.voice, samples.Output()).error == &AudioErrors::HandleStale);
        }

        TEST_CASE("Stack and restart share real group limits and successful target cooldown", "[audio][repeated_playback]") {
            auto owner = Owner(4);
            auto firstBinding = Binding();
            auto secondBinding = Binding(false, 2, 2);
            const AudioConcurrencyGroup group{AudioConcurrencyGroupId::Create(557).Value(), AudioConcurrencyScope::Global, 1, 10};
            firstBinding.prototype.playback.concurrency.group = group.group;
            secondBinding.prototype.playback.concurrency.group = group.group;
            const auto firstLane = owner.Bind(firstBinding, {AudioRetriggerPolicy::Restart, group, {}}).Value();
            const auto secondLane = owner.Bind(secondBinding, {AudioRetriggerPolicy::Stack, group, {}}).Value();
            Samples samples;
            const auto first = Submit(owner, Request(firstLane, firstBinding, 1), samples);
            CHECK(Submit(owner, Request(secondLane, secondBinding, 1), samples).disposition ==
                  AudioRepeatedPlaybackDisposition::InstanceLimit);
            Apply(owner, first);
            CHECK(Submit(owner, Request(firstLane, firstBinding, 2), samples, false, 9).disposition ==
                  AudioRepeatedPlaybackDisposition::RetriggerWindow);
            const auto restarted = Submit(owner, Request(firstLane, firstBinding, 3), samples, false, 10);
            CHECK(restarted.disposition == AudioRepeatedPlaybackDisposition::Restarted);
            Retire(owner, restarted);
            const auto blocked = Submit(owner, Request(secondLane, secondBinding, 2), samples, false, 19);
            CHECK(blocked.disposition == AudioRepeatedPlaybackDisposition::RetriggerWindow);
            CHECK(blocked.concurrency.remainingRetriggerFrames == 1);
            const auto admitted = Submit(owner, Request(secondLane, secondBinding, 3), samples, false, 20);
            CHECK(admitted.disposition == AudioRepeatedPlaybackDisposition::Admitted);
        }

        TEST_CASE("Emitter and owner generations partition groups while local limits remain enforced", "[audio][repeated_playback]") {
            for (const auto scope : {AudioConcurrencyScope::Emitter, AudioConcurrencyScope::Owner}) {
                auto owner = Owner(4);
                auto a = Binding();
                auto b = Binding();
                if (scope == AudioConcurrencyScope::Emitter)
                    b.emitter.generation = 2;
                else
                    b.owner.generation = 2;
                const AudioConcurrencyGroup group{AudioConcurrencyGroupId::Create(558).Value(), scope, 1, 0};
                a.prototype.playback.concurrency = {group.group, 1, AudioConcurrencyMode::Reject};
                b.prototype.playback.concurrency = a.prototype.playback.concurrency;
                const auto first = owner.Bind(a, {.group = group}).Value();
                const auto second = owner.Bind(b, {.group = group}).Value();
                Samples samples;
                CHECK(Submit(owner, Request(first, a, 1), samples).disposition == AudioRepeatedPlaybackDisposition::Admitted);
                CHECK(Submit(owner, Request(second, b, 1), samples).disposition == AudioRepeatedPlaybackDisposition::Admitted);
                CHECK(Submit(owner, Request(first, a, 2), samples).disposition == AudioRepeatedPlaybackDisposition::InstanceLimit);
            }
            auto owner = Owner();
            auto binding = Binding();
            binding.prototype.playback.concurrency.maxInstances = 1;
            const auto lane = owner.Bind(binding, {}).Value();
            Samples samples;
            CHECK(Submit(owner, Request(lane, binding, 1), samples).disposition == AudioRepeatedPlaybackDisposition::Admitted);
            CHECK(Submit(owner, Request(lane, binding, 2), samples).disposition == AudioRepeatedPlaybackDisposition::InstanceLimit);
        }

        TEST_CASE("Sample targets survive staging and reject early late stale forged and reset commands", "[audio][repeated_playback]") {
            auto owner = Owner();
            const auto binding = Binding();
            const auto lane = owner.Bind(binding, {}).Value();
            Samples samples;
            auto request = Request(lane, binding, 1);
            request.target = {AudioCommandTargetKind::ExactSampleFrame, 100, 1, 1};
            auto receipt = Submit(owner, request, samples);
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Ready);
            CHECK(owner.Render(receipt.voice, samples.Output()).produced == 0);
            CHECK(owner.Apply(receipt.commands.commands[0], {1, 99}) == &AudioErrors::ConcurrencyTimelineStale);
            CHECK(owner.Apply(receipt.commands.commands[0], {1, 101}) == &AudioErrors::ConcurrencyTimelineStale);
            auto forged = receipt.commands.commands[0];
            forged.scope.scene.generation++;
            CHECK(owner.Apply(forged, {1, 100}) == &AudioErrors::PlaybackRequestInvalid);
            Apply(owner, receipt, 100);
            CHECK(owner.Apply(receipt.commands.commands[0], {1, 100}) == &AudioErrors::HandleStale);
            const auto cached = owner.Submit(request, {}, {1, 110});
            REQUIRE(cached.HasValue());
            CHECK(cached.Value().disposition == AudioRepeatedPlaybackDisposition::Replay);
            CHECK(cached.Value().voice == receipt.voice);
            CHECK(cached.Value().commands.commandCount == 0);
            SameChoice(receipt, cached.Value());
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Playing);
            CHECK(owner.Submit(request, {}, {1, 109}).HasError());
            auto retrograde = Request(lane, binding, 2);
            CHECK(owner.Submit(retrograde, samples.Resolved(false), {1, 109}).HasError());
            CHECK(owner.Submit(request, {}, {2, 110}).HasError());
            auto lateNewRequest = request;
            lateNewRequest.sequence = 2;
            CHECK(owner.Submit(lateNewRequest, samples.Resolved(false), {1, 110}).HasError());
            REQUIRE(owner.Reset(2, 2, 2).HasValue());
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Cancelled);
            CHECK(owner.Submit(request, samples.Resolved(false), {2, 0}).HasError());
            CHECK(owner.Apply(receipt.commands.commands[0], {2, 0}) == &AudioErrors::HandleStale);
            REQUIRE(owner.Release(receipt.voice).HasValue());
            const auto fresh = owner.Bind(binding, {}).Value();
            CHECK(fresh.generation != lane.generation);
            auto freshRequest = Request(fresh, binding, 1);
            auto newReceipt = owner.Submit(freshRequest, samples.Resolved(false), {2, 0});
            REQUIRE(newReceipt.HasValue());
            CHECK(newReceipt.Value().voice.generation != receipt.voice.generation);
            CHECK(owner.Reset(2, 2, 2).HasError());
        }

        TEST_CASE("Scene unload closes exact lanes while retaining terminal evidence", "[audio][repeated_playback]") {
            auto owner = Owner();
            const auto a = Binding();
            const auto b = Binding(false, 2, 2, 2);
            const auto first = owner.Bind(a, {}).Value();
            const auto second = owner.Bind(b, {}).Value();
            Samples samples;
            const auto admitted = Submit(owner, Request(first, a, 1), samples);
            REQUIRE(owner.RetireScene(a.prototype.sceneContext).HasValue());
            CHECK(owner.Snapshot(admitted.voice).Value().terminalReason == AudioVoiceTerminalReason::Cancelled);
            CHECK(owner.Submit(Request(first, a, 2), samples.Resolved(false), {1, 0}).HasError());
            CHECK(owner.Bind(a, {}).HasError());
            CHECK(Submit(owner, Request(second, b, 1), samples).disposition == AudioRepeatedPlaybackDisposition::Admitted);
            CHECK(owner.Release(admitted.voice).HasValue());
        }

        TEST_CASE("Malformed authored variation and resolver inputs leave admission state untouched", "[audio][repeated_playback]") {
            auto owner = Owner();
            const auto binding = Binding(true);
            auto variation = Variation(AudioVariationSelection::WeightedRandom);
            for (const auto weight : {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                auto malformed = variation;
                malformed.entries[0].weight = weight;
                CHECK(owner.Bind(binding, {.variation = malformed}).HasError());
            }
            auto malformed = variation;
            malformed.entries.clear();
            CHECK(owner.Bind(binding, {.variation = malformed}).HasError());
            malformed = variation;
            malformed.entries[0].clip = malformed.entries[1].clip;
            CHECK(owner.Bind(binding, {.variation = malformed}).HasError());
            malformed = variation;
            malformed.entries.resize(MaximumAudioVariationEntries + 1);
            CHECK(owner.Bind(binding, {.variation = malformed}).HasError());
            malformed = variation;
            malformed.pitchDeltaSemitones.maximum = std::numeric_limits<float>::quiet_NaN();
            CHECK(owner.Bind(binding, {.variation = malformed}).HasError());
            const auto lane = owner.Bind(binding, {.variation = variation}).Value();
            Samples samples;
            auto clips = samples.clips;
            clips[2].clip = clips[0].clip;
            CHECK(owner.Submit(Request(lane, binding, 1), clips, {1, 0}).HasError());
            CHECK(owner.Submit(Request(lane, binding, 1), std::span{clips}.first(1), {1, 0}).HasError());
            clips = samples.clips;
            clips[1].pcm.frames = 513;
            CHECK(owner.Submit(Request(lane, binding, 1), clips, {1, 0}).HasError());
            CHECK(Submit(owner, Request(lane, binding, 1), samples, true).disposition == AudioRepeatedPlaybackDisposition::Admitted);
        }

        TEST_CASE("Preparation and range overflow failures do not consume selection or canonical slots", "[audio][repeated_playback]") {
            auto owner = Owner(1);
            auto replay = Owner(1);
            const auto binding = Binding(true);
            const auto variation = Variation(AudioVariationSelection::Random);
            const auto lane = owner.Bind(binding, {.variation = variation}).Value();
            const auto replayLane = replay.Bind(binding, {.variation = variation}).Value();
            Samples samples;
            auto invalid = Request(lane, binding, 1);
            invalid.playback.playback.pitch = 8;
            // Fixed positive octave exceeds (0,8] regardless of the seed.
            auto overflow = Variation(AudioVariationSelection::Random);
            overflow.pitchDeltaSemitones = {12, 12};
            auto overflowBinding = binding;
            overflowBinding.incarnation = 2;
            const auto overflowLane = owner.Bind(overflowBinding, {.variation = overflow}).Value();
            invalid.lane = overflowLane;
            CHECK(owner.Submit(invalid, samples.Resolved(true), {1, 0}).HasError());
            bool failed{};
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure;
                failed = owner.Submit(Request(lane, binding, 1), samples.Resolved(true), {1, 0}).HasError();
            }
            CHECK(failed);
            const auto accepted = Submit(owner, Request(lane, binding, 1), samples, true);
            SameChoice(accepted, Submit(replay, Request(replayLane, binding, 1), samples, true));
        }

        TEST_CASE("One-shot render drains once and command processing performs no allocation", "[audio][repeated_playback]") {
            auto owner = Owner(1);
            const auto binding = Binding();
            const auto lane = owner.Bind(binding, {}).Value();
            Samples samples;
            samples.clips[0].pcm.frames = 8;
            const auto receipt = Submit(owner, Request(lane, binding, 1), samples);
            const auto before = Horo::Tests::AllocationProbe::Count();
            bool applied{};
            AudioVoiceRenderResult rendered;
            {
                applied = owner.Apply(receipt.commands.commands[0], {1, 0}) == nullptr;
                rendered = owner.Render(receipt.voice, samples.Output());
            }
            const auto after = Horo::Tests::AllocationProbe::Count();
            CHECK(applied);
            CHECK(rendered.error == nullptr);
            CHECK(rendered.terminal);
            CHECK(after == before);
            CHECK(owner.Snapshot(receipt.voice).Value().terminalReason == AudioVoiceTerminalReason::Finished);
            CHECK_FALSE(owner.Render(receipt.voice, samples.Output()).terminal);
            CHECK(owner.Release(receipt.voice).HasValue());
        }

        struct RepeatedPlaybackPort final {
            AudioRepeatedPlayback &owner;
            AudioCommandBuffer &commands;
            AudioVoiceHandle voice;
            std::uint64_t frame{};
            std::uint32_t terminals{};
            float firstSample{};

            static Backend::RenderResult Process(void *context, const Backend::RenderInvocation &invocation) noexcept {
                auto &port = *static_cast<RepeatedPlaybackPort *>(context);
                using enum Backend::RenderDisposition;
                if (invocation.phase != Backend::RenderPhase::Rendering)
                    return PlaybackTest::SilentPhase(invocation);
                AudioCommandRecord record;
                for (std::uint32_t count = 0; count < 4 && port.commands.TryConsume(record); ++count) {
                    if (record.command.scope.epoch != invocation.epoch.callbackEpoch || port.owner.Apply(record.command, {1, port.frame}))
                        return {};
                }
                std::array<std::span<float>, 1> planes{std::span{invocation.output.planes[0], invocation.output.capacityFrames}};
                const auto rendered = port.owner.Render(port.voice, {planes, invocation.output.validFrames});
                port.firstSample = planes[0][0];
                port.terminals += rendered.terminal ? 1U : 0U;
                port.frame += invocation.output.validFrames;
                return rendered.error ? Backend::RenderResult{} : Backend::RenderResult{Rendered, AudioCallbackFaultCode::None};
            }
        };

        TEST_CASE("Repeated requests produce real PCM through normalized SPSC controls in Null callbacks",
                  "[audio][repeated_playback][null]") {
            auto owner = Owner(1);
            const auto binding = Binding(true);
            auto variation = Variation(AudioVariationSelection::WeightedRandom);
            variation.pitchDeltaSemitones = {12, 12};
            variation.gainDeltaDb = {-6, -6};
            const auto lane = owner.Bind(binding, {AudioRetriggerPolicy::Restart, {}, variation}).Value();
            Samples samples;
            for (auto &clip : samples.clips)
                clip.pcm.frames = 400;
            const auto receipt = Submit(owner, Request(lane, binding, 1), samples, true);
            auto commands =
                std::move(AudioCommandBuffer::Create({Runtime(), AudioMemoryPoolId::Create(557).Value(), 1, 4, 1, 16384}).Value());
            RepeatedPlaybackPort port{owner, commands, receipt.voice};
            auto backend = std::move(Backend::CreateNullAudioBackend({Runtime(), 1, 1, 1}).Value());
            const AudioDeviceEpoch epoch{{Runtime(), 1, 1}, 1, 1};
            PlaybackTest::Start(*backend, epoch, {&port, RepeatedPlaybackPort::Process});
            REQUIRE(commands.TryPublish({1, receipt.commands.commands[0]}) == AudioCommandPublishStatus::Published);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.firstSample == Catch::Approx(receipt.gain * 0.25F).epsilon(2e-6));
            const auto restart = Submit(owner, Request(lane, binding, 2), samples, true, port.frame);
            CHECK(restart.voice == receipt.voice);
            REQUIRE(commands.TryPublish({2, restart.commands.commands[0]}) == AudioCommandPublishStatus::Published);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.terminals == 0);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.terminals == 1);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(port.terminals == 1);
            CHECK(commands.Depth() == 0);
            PlaybackTest::Shutdown(*backend, commands, epoch);
            REQUIRE(owner.Release(receipt.voice).HasValue());
        }

        TEST_CASE("Version one replay has frozen first random selection and dyadic adjustment bits", "[audio][repeated_playback]") {
            static_assert(AudioPlaybackReplayVersion == 1);
            auto owner = Owner(1);
            const auto binding = Binding(true);
            const auto lane = owner.Bind(binding, {.variation = Variation(AudioVariationSelection::Random)}).Value();
            Samples samples;
            const auto receipt = Submit(owner, Request(lane, binding, 1), samples, true);
            CHECK(receipt.clip == Clip(2));
            CHECK(receipt.pitchDeltaSemitones == static_cast<float>(-2.0 + 4.0 * (2682851.0 / 16777215.0)));
            CHECK(receipt.gainDeltaDb == static_cast<float>(-6.0 + 6.0 * (4674151.0 / 16777215.0)));
        }

        TEST_CASE("Pause ramp restart rejection and paused group counting preserve admission history", "[audio][repeated_playback]") {
            auto owner = Owner(2);
            auto binding = Binding();
            const AudioConcurrencyGroup group{AudioConcurrencyGroupId::Create(559).Value(),
                                              AudioConcurrencyScope::Global,
                                              1,
                                              10,
                                              false,
                                              true};
            binding.prototype.playback.concurrency.group = group.group;
            const auto lane = owner.Bind(binding, {AudioRetriggerPolicy::Restart, group, {}}).Value();
            Samples samples;
            const auto receipt = Submit(owner, Request(lane, binding, 1), samples);
            Apply(owner, receipt);
            REQUIRE(owner.Render(receipt.voice, samples.Output(8)).error == nullptr);
            const AudioCommand pause{{Runtime(), 1, binding.prototype.sceneContext},
                                     AudioVoiceControlRequest{receipt.voice, AudioVoiceControl::Pause}};
            REQUIRE(owner.Apply(pause, {1, 8}) == nullptr);
            CHECK(owner.Submit(Request(lane, binding, 2), samples.Resolved(false), {1, 10}).HasError());
            REQUIRE(owner.Render(receipt.voice, samples.Output(4)).error == nullptr);
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Paused);
            const auto restart = Submit(owner, Request(lane, binding, 2), samples, false, 10);
            CHECK(restart.disposition == AudioRepeatedPlaybackDisposition::Restarted);
            Apply(owner, restart, 10);
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Playing);
            REQUIRE(owner.Render(receipt.voice, samples.Output(4)).error == nullptr);
            CHECK(samples.output[0] == 0.25F);
        }

        TEST_CASE("Future successful target reservations prevent backwards bucket cooldown", "[audio][repeated_playback]") {
            auto owner = Owner();
            auto a = Binding();
            auto b = Binding(false, 2, 2);
            const AudioConcurrencyGroup group{AudioConcurrencyGroupId::Create(560).Value(), AudioConcurrencyScope::Global, 0, 10};
            a.prototype.playback.concurrency.group = group.group;
            b.prototype.playback.concurrency.group = group.group;
            const auto first = owner.Bind(a, {.group = group}).Value();
            const auto second = owner.Bind(b, {.group = group}).Value();
            Samples samples;
            auto request = Request(first, a, 1);
            request.target = {AudioCommandTargetKind::ExactSampleFrame, 100, 1, 1};
            const auto admitted = Submit(owner, request, samples);
            CHECK(owner.Submit(Request(second, b, 1), samples.Resolved(false), {1, 0}).HasError());
            auto next = Request(second, b, 1);
            next.target = {AudioCommandTargetKind::ExactSampleFrame, 109, 1, 1};
            CHECK(Submit(owner, next, samples).disposition == AudioRepeatedPlaybackDisposition::RetriggerWindow);
            next.sequence = 2;
            next.target.sampleFrame = 110;
            CHECK(Submit(owner, next, samples).disposition == AudioRepeatedPlaybackDisposition::Admitted);
            REQUIRE(owner.Cancel(admitted.voice).HasValue());
            CHECK(owner.Apply(admitted.commands.commands[0], {1, 100}) == &AudioErrors::HandleStale);
        }

        TEST_CASE("Singleton and maximum bounded shuffle containers admit without repeat assumptions", "[audio][repeated_playback]") {
            for (const auto selection : {AudioVariationSelection::Random, AudioVariationSelection::RoundRobin,
                                         AudioVariationSelection::Shuffle, AudioVariationSelection::WeightedRandom}) {
                auto owner = Owner(1);
                const auto binding = Binding(true);
                auto variation = Variation(selection);
                variation.entries = {{Clip(1), std::numeric_limits<float>::denorm_min()}};
                if (selection != AudioVariationSelection::WeightedRandom)
                    variation.entries[0].weight = 1;
                variation.pitchDeltaSemitones = {};
                variation.gainDeltaDb = {};
                variation.deterministicSeed = 0;
                const auto lane = owner.Bind(binding, {.variation = variation}).Value();
                Samples samples;
                for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
                    const auto result = owner.Submit(Request(lane, binding, sequence), samples.Resolved(false), {1, 0});
                    REQUIRE(result.HasValue());
                    CHECK(result.Value().clip == Clip(1));
                    CHECK(result.Value().pitch == 1);
                    CHECK(result.Value().gain == 1);
                    Retire(owner, result.Value());
                }
            }
            auto owner = Owner(1);
            const auto binding = Binding(true);
            auto variation = Variation(AudioVariationSelection::Shuffle);
            variation.entries.clear();
            Samples samples;
            std::vector<AudioResolvedPlaybackClip> clips;
            for (std::uint32_t index = 1; index <= MaximumAudioVariationEntries; ++index) {
                auto bytes = Clip(1).Asset().Bytes();
                bytes[14] = static_cast<std::uint8_t>(index >> 8);
                bytes[15] = static_cast<std::uint8_t>(index);
                const auto clip = AudioClipId::Create(Assets::AssetId::FromBytes(bytes)).Value();
                variation.entries.push_back({clip, 1});
                clips.push_back({clip, {samples.inputs, 8, true}, 48000});
            }
            const auto lane = owner.Bind(binding, {.variation = variation}).Value();
            std::vector<AudioClipId> selected;
            for (std::uint64_t sequence = 1; sequence <= 10; ++sequence) {
                const auto result = owner.Submit(Request(lane, binding, sequence), clips, {1, 0});
                REQUIRE(result.HasValue());
                CHECK(std::ranges::find(selected, result.Value().clip) == selected.end());
                selected.push_back(result.Value().clip);
                Retire(owner, result.Value());
            }
        }
    }  // namespace
}  // namespace Horo::Audio
