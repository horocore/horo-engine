#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioVoiceStateMachine.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] AudioRuntimeId Owner(const std::uint64_t value = 77) {
            return AudioRuntimeId::Create(value).Value();
        }

        [[nodiscard]] AudioVoiceStateMachine Prepared(const std::uint32_t maximumVoices = 4, const std::uint32_t maximumGeneration = 8) {
            auto created =
                AudioVoiceStateMachine::Create({.owner = Owner(), .maximumVoices = maximumVoices, .maximumGeneration = maximumGeneration});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Audio voice state machine follows the complete playback lifecycle", "[unit][audio][voice]") {
            auto voices = Prepared();
            const auto created = voices.CreateVoice();
            REQUIRE(created.HasValue());
            const auto voice = created.Value();
            CHECK(voices.State(voice).Value() == AudioVoiceState::Created);

            REQUIRE(voices.Transition(voice, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Paused).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Virtual).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Stopping).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Stopped).HasValue());

            CHECK(IsTerminalAudioVoiceState(voices.State(voice).Value()));
            CHECK(AudioVoiceTerminalReasonForState(voices.State(voice).Value()).value() == AudioVoiceTerminalReason::Stopped);
            RequireError(voices.Transition(voice, AudioVoiceState::Playing), AudioErrors::VoiceInvalidTransition);
        }

        TEST_CASE("Audio voice state machine rejects invalid transitions without mutation", "[unit][audio][voice]") {
            auto voices = Prepared();
            const auto voice = voices.CreateVoice().Value();

            RequireError(voices.Transition(voice, AudioVoiceState::Playing), AudioErrors::VoiceInvalidTransition);
            CHECK(voices.State(voice).Value() == AudioVoiceState::Created);
            RequireError(voices.Transition(voice, AudioVoiceState::Paused), AudioErrors::VoiceInvalidTransition);
            CHECK(voices.State(voice).Value() == AudioVoiceState::Created);

            const AudioVoiceHandle foreign{Owner(78), voice.slot, voice.generation};
            RequireError(voices.Transition(foreign, AudioVoiceState::Ready), AudioErrors::HandleOwnerMismatch);
        }

        TEST_CASE("Audio voice state machine preserves natural completion and direct stops", "[unit][audio][voice]") {
            auto voices = Prepared(3);
            const auto finished = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(finished, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(finished, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(finished, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.Transition(finished, AudioVoiceState::Finished).HasValue());
            CHECK(AudioVoiceTerminalReasonForState(voices.State(finished).Value()).value() == AudioVoiceTerminalReason::Finished);
            REQUIRE(voices.Release(finished).HasValue());

            const auto scheduledStop = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(scheduledStop, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(scheduledStop, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(scheduledStop, AudioVoiceState::Stopped).HasValue());

            const auto playingStop = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(playingStop, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(playingStop, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(playingStop, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.Transition(playingStop, AudioVoiceState::Stopped).HasValue());
        }

        TEST_CASE("Audio voice cancellation and shutdown produce one terminal state", "[unit][audio][voice][shutdown]") {
            auto voices = Prepared(3);
            const auto cancelled = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(cancelled, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Cancel(cancelled).HasValue());
            CHECK(voices.State(cancelled).Value() == AudioVoiceState::Cancelled);
            RequireError(voices.Cancel(cancelled), AudioErrors::VoiceInvalidTransition);
            REQUIRE(voices.Release(cancelled).HasValue());

            const auto first = voices.CreateVoice().Value();
            const auto second = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(first, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(first, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(first, AudioVoiceState::Playing).HasValue());
            REQUIRE(voices.BeginShutdown().HasValue());
            CHECK(voices.State(first).Value() == AudioVoiceState::Cancelled);
            CHECK(voices.State(second).Value() == AudioVoiceState::Cancelled);
            RequireError(voices.CreateVoice(), AudioErrors::VoiceAdmissionClosed);
            REQUIRE(voices.Release(first).HasValue());
            REQUIRE(voices.Release(second).HasValue());
            REQUIRE(voices.BeginShutdown().HasValue());
        }

        TEST_CASE("Stale voice generations cannot mutate replacement voices", "[unit][audio][voice][generation]") {
            auto voices = Prepared(1);
            const auto original = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(original, AudioVoiceState::Failed).HasValue());
            REQUIRE(voices.Release(original).HasValue());

            const auto replacement = voices.CreateVoice().Value();
            REQUIRE(replacement.slot == original.slot);
            REQUIRE(replacement.generation == original.generation + 1);
            RequireError(voices.Transition(original, AudioVoiceState::Ready), AudioErrors::HandleStale);
            CHECK(voices.State(replacement).Value() == AudioVoiceState::Created);
        }

        TEST_CASE("Audio voice steady-state transitions do not allocate", "[unit][audio][voice][realtime]") {
            auto voices = Prepared(2);
            const auto voice = voices.CreateVoice().Value();

            Horo::Tests::AllocationProbe::ScopedFailure failure;
            const auto ready = voices.Transition(voice, AudioVoiceState::Ready);
            const auto scheduled = voices.Transition(voice, AudioVoiceState::Scheduled);
            const auto playing = voices.Transition(voice, AudioVoiceState::Playing);
            const auto paused = voices.Transition(voice, AudioVoiceState::Paused);
            const auto resumed = voices.Transition(voice, AudioVoiceState::Playing);
            const auto snapshot = voices.Snapshot(voice);
            REQUIRE(ready.HasValue());
            REQUIRE(scheduled.HasValue());
            REQUIRE(playing.HasValue());
            REQUIRE(paused.HasValue());
            REQUIRE(resumed.HasValue());
            REQUIRE(snapshot.HasValue());
        }
    }  // namespace
}  // namespace Horo::Audio
