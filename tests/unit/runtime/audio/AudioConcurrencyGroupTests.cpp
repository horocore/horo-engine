#include "AllocationProbe.h"
#include "Horo/Audio/AudioConcurrencyGroup.h"
#include "Horo/Audio/AudioErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] AudioRuntimeId Runtime(const std::uint64_t value = 71) {
            return AudioRuntimeId::Create(value).Value();
        }

        [[nodiscard]] AudioConcurrencyGroup Group(const AudioConcurrencyScope scope = AudioConcurrencyScope::Global) {
            return {.group = AudioConcurrencyGroupId::Create(9).Value(), .scope = scope, .maximumInstances = 2, .retriggerFrames = 10};
        }

        [[nodiscard]] AudioConcurrencyRequest Request() {
            return {.runtime = Runtime(), .emitter = {Runtime(), 1, 1}, .owner = {Runtime(), 2, 1}};
        }

        [[nodiscard]] AudioConcurrencySnapshot Snapshot(const AudioConcurrencyGroup &group, const AudioConcurrencyRequest &request) {
            return {.key = MakeAudioConcurrencyKey(group, request).Value(), .timelineGeneration = 1};
        }

        [[nodiscard]] AudioVoiceSnapshot Voice(const std::uint32_t slot, const AudioVoiceState state = AudioVoiceState::Playing) {
            return {.voice = {Runtime(), slot, 1}, .state = state, .terminalReason = AudioVoiceTerminalReasonForState(state)};
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            INFO("Expected " << error.code.Value() << "; actual diagnostic: " << result.ErrorValue().message);
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        TEST_CASE("Concurrency keys partition global emitter and owner identities", "[unit][audio][concurrency]") {
            const auto request = Request();
            auto other = request;
            other.emitter.slot = 7;
            other.owner.slot = 8;
            for (const auto scope : {AudioConcurrencyScope::Global, AudioConcurrencyScope::Emitter, AudioConcurrencyScope::Owner}) {
                const auto group = Group(scope);
                const auto key = MakeAudioConcurrencyKey(group, request).Value();
                CHECK((key == MakeAudioConcurrencyKey(group, other).Value()) == (scope == AudioConcurrencyScope::Global));
                auto nextGeneration = request;
                ++nextGeneration.emitter.generation;
                ++nextGeneration.owner.generation;
                CHECK((key == MakeAudioConcurrencyKey(group, nextGeneration).Value()) == (scope == AudioConcurrencyScope::Global));
                CHECK(key.emitter.IsValid() == (scope == AudioConcurrencyScope::Emitter));
                CHECK(key.owner.IsValid() == (scope == AudioConcurrencyScope::Owner));
            }
            auto globalRequest = request;
            globalRequest.emitter = {};
            globalRequest.owner = {};
            CHECK(MakeAudioConcurrencyKey(Group(), globalRequest).HasValue());
            auto otherRuntime = request;
            otherRuntime.runtime = Runtime(72);
            CHECK(MakeAudioConcurrencyKey(Group(), request).Value() != MakeAudioConcurrencyKey(Group(), otherRuntime).Value());
        }

        TEST_CASE("Scoped concurrency rejects another emitter or owner bucket during admission", "[unit][audio][concurrency]") {
            for (const auto scope : {AudioConcurrencyScope::Emitter, AudioConcurrencyScope::Owner}) {
                const auto group = Group(scope);
                const auto request = Request();
                auto snapshot = Snapshot(group, request);
                const std::array voices{Voice(1), Voice(2)};
                snapshot.voices = voices;
                CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 50}).Value().eligibility ==
                      AudioConcurrencyEligibility::InstanceLimit);
                auto replacement = request;
                ++replacement.emitter.generation;
                ++replacement.owner.generation;
                CheckError(EvaluateAudioConcurrency(group, replacement, snapshot, {1, 50}), AudioErrors::ConcurrencyInvalid);
                const auto fresh = Snapshot(group, replacement);
                CHECK(EvaluateAudioConcurrency(group, replacement, fresh, {1, 50}).Value().eligibility ==
                      AudioConcurrencyEligibility::Eligible);
            }
        }

        TEST_CASE("Concurrency group and request validation fail closed", "[unit][audio][concurrency]") {
            auto group = Group();
            auto request = Request();
            group.group = {};
            CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::ConcurrencyInvalid);
            group = Group();
            group.scope = static_cast<AudioConcurrencyScope>(255);
            CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::ConcurrencyInvalid);
            group = Group();
            group.maximumInstances = MaximumAudioVoiceSlots + 1;
            CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::ConcurrencyInvalid);
            group.maximumInstances = MaximumAudioVoiceSlots;
            CHECK(MakeAudioConcurrencyKey(group, request).HasValue());
            request.runtime = {};
            CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::ConcurrencyInvalid);
            for (const auto scope : {AudioConcurrencyScope::Emitter, AudioConcurrencyScope::Owner}) {
                group = Group(scope);
                request = {.runtime = Runtime()};
                CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::ConcurrencyInvalid);
                request = Request();
                request.emitter.owner = Runtime(72);
                request.owner.owner = Runtime(72);
                CheckError(MakeAudioConcurrencyKey(group, request), AudioErrors::HandleOwnerMismatch);
            }
        }

        TEST_CASE("Concurrency counts registry lifecycle states with authored pause and virtualization rules",
                  "[unit][audio][concurrency]") {
            const auto request = Request();
            auto group = Group();
            group.retriggerFrames = 0;
            auto snapshot = Snapshot(group, request);
            for (const auto state : {AudioVoiceState::Created, AudioVoiceState::Ready, AudioVoiceState::Scheduled, AudioVoiceState::Playing,
                                     AudioVoiceState::Paused, AudioVoiceState::Virtual, AudioVoiceState::Stopping, AudioVoiceState::Stopped,
                                     AudioVoiceState::Finished, AudioVoiceState::Cancelled, AudioVoiceState::Failed}) {
                const std::array voices{Voice(1, state), Voice(2, state)};
                snapshot.voices = voices;
                const auto decision = EvaluateAudioConcurrency(group, request, snapshot, {1, 0});
                REQUIRE(decision.HasValue());
                CHECK(decision.Value().countedInstances == (IsTerminalAudioVoiceState(state) ? 0 : 2));
                CHECK(decision.Value().eligibility == (IsTerminalAudioVoiceState(state) ? AudioConcurrencyEligibility::Eligible
                                                                                        : AudioConcurrencyEligibility::InstanceLimit));
            }
            const std::array pausedAndVirtual{Voice(1, AudioVoiceState::Paused), Voice(2, AudioVoiceState::Virtual)};
            snapshot.voices = pausedAndVirtual;
            group.countPaused = false;
            group.countVirtual = false;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().countedInstances == 0);
            group.countPaused = true;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().countedInstances == 1);
            group.countPaused = false;
            group.countVirtual = true;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().countedInstances == 1);
            group.maximumInstances = 0;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().eligibility == AudioConcurrencyEligibility::Eligible);
        }

        TEST_CASE("Concurrency cooldown survives completion and uses overflow-safe inclusive boundaries", "[unit][audio][concurrency]") {
            const auto request = Request();
            auto group = Group();
            auto snapshot = Snapshot(group, request);
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().eligibility == AudioConcurrencyEligibility::Eligible);
            snapshot.lastAdmissionFrame = 0;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}).Value().remainingRetriggerFrames == 10);
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 9}).Value().eligibility ==
                  AudioConcurrencyEligibility::RetriggerWindow);
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 10}).Value().eligibility == AudioConcurrencyEligibility::Eligible);
            const std::array voices{Voice(1), Voice(2)};
            snapshot.voices = voices;
            const auto both = EvaluateAudioConcurrency(group, request, snapshot, {1, 5}).Value();
            CHECK(both.eligibility == AudioConcurrencyEligibility::InstanceLimit);
            CHECK(both.remainingRetriggerFrames == 5);
            snapshot.voices = {};
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            group.retriggerFrames = maximum;
            snapshot.lastAdmissionFrame = maximum - 1;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, maximum}).Value().remainingRetriggerFrames == maximum - 1);
            group.retriggerFrames = 0;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, maximum}).Value().eligibility ==
                  AudioConcurrencyEligibility::Eligible);
        }

        TEST_CASE("Concurrency rejects stale timelines wrong buckets and malformed registry projections", "[unit][audio][concurrency]") {
            const auto group = Group();
            const auto request = Request();
            auto snapshot = Snapshot(group, request);
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {2, 0}), AudioErrors::ConcurrencyTimelineStale);
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {0, 0}), AudioErrors::ConcurrencyInvalid);
            snapshot.lastAdmissionFrame = 1;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyTimelineStale);
            snapshot.lastAdmissionFrame.reset();
            snapshot.key.group = AudioConcurrencyGroupId::Create(10).Value();
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            snapshot = Snapshot(group, request);
            snapshot.key.emitter = request.emitter;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            snapshot = Snapshot(group, request);
            std::array voices{Voice(1), Voice(2)};
            snapshot.voices = voices;
            voices[1] = voices[0];
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices = {Voice(1), Voice(1)};
            voices[1].voice.generation = 2;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices = {Voice(2), Voice(1)};
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices = {Voice(1), Voice(2)};
            voices[0].voice.owner = Runtime(72);
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::HandleOwnerMismatch);
            voices[0] = Voice(1, static_cast<AudioVoiceState>(255));
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices[0] = Voice(1, AudioVoiceState::Finished);
            voices[0].terminalReason.reset();
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices[0] = Voice(1);
            voices[0].voice.generation = 0;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            voices = {Voice(1), Voice(2)};
            voices[1].voice.slot = MaximumAudioVoiceSlots + 1;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            snapshot.timelineGeneration = 0;
            CheckError(EvaluateAudioConcurrency(group, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
            auto invalidGroup = group;
            invalidGroup.group = {};
            CheckError(EvaluateAudioConcurrency(invalidGroup, request, snapshot, {1, 0}), AudioErrors::ConcurrencyInvalid);
        }

        TEST_CASE("Concurrency replay observes admitted request order without allocation or mutation", "[unit][audio][concurrency]") {
            const auto group = Group();
            const auto request = Request();
            auto voices = AudioVoiceStateMachine::Create({.owner = Runtime(), .maximumVoices = 2}).Value();
            auto snapshot = Snapshot(group, request);
            std::array<AudioConcurrencyDecision, 4> firstReplay{};
            for (int replay = 0; replay < 2; ++replay) {
                snapshot.lastAdmissionFrame.reset();
                for (std::size_t index = 0; index < firstReplay.size(); ++index) {
                    const AudioConcurrencyTime time{1, index * 5};
                    const auto result = EvaluateAudioConcurrency(group, request, snapshot, time);
                    REQUIRE(result.HasValue());
                    if (replay == 0)
                        firstReplay[index] = result.Value();
                    else
                        CHECK(result.Value() == firstReplay[index]);
                    if (result.Value().eligibility == AudioConcurrencyEligibility::Eligible)
                        snapshot.lastAdmissionFrame = time.sampleFrame;
                }
            }
            const auto handle = voices.CreateVoice().Value();
            const std::array projection{voices.Snapshot(handle).Value()};
            snapshot.voices = projection;
            const auto before = Tests::AllocationProbe::Count();
            const auto evaluated = EvaluateAudioConcurrency(group, request, snapshot, {1, 20});
            const auto allocations = Tests::AllocationProbe::Count() - before;
            REQUIRE(evaluated.HasValue());
            CHECK(allocations == 0);
            CHECK(evaluated.Value().countedInstances == 1);
            REQUIRE(voices.Cancel(handle).HasValue());
            const std::array terminal{voices.Snapshot(handle).Value()};
            snapshot.voices = terminal;
            CHECK(EvaluateAudioConcurrency(group, request, snapshot, {1, 20}).Value().countedInstances == 0);
        }
    }  // namespace
}  // namespace Horo::Audio
