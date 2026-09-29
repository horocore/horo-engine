#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioFailureRecovery.h"
#include "Horo/Audio/AudioVoiceStateMachine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Audio {
    namespace {
        TEST_CASE("Audio failures classify every owned boundary without mutating published state", "[unit][audio][failure]") {
            struct Case final {
                const ErrorCodeDescriptor *descriptor;
                AudioFailureArea area;
                AudioFailureState state;
                AudioRecoveryAction action;
                AudioActiveStatePolicy active;
            };

            const std::array cases{
                Case{&AudioErrors::AssetSchemaInvalid, AudioFailureArea::Asset, AudioFailureState::Rejected,
                     AudioRecoveryAction::CorrectInput, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::SourceDecodeFailed, AudioFailureArea::Codec, AudioFailureState::Rejected,
                     AudioRecoveryAction::CorrectInput, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::StreamUnderrun, AudioFailureArea::Stream, AudioFailureState::Recovering,
                     AudioRecoveryAction::RefillStream, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::QueueSaturated, AudioFailureArea::Queue, AudioFailureState::Rejected,
                     AudioRecoveryAction::RetryAtSafePoint, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::VoiceInvalidTransition, AudioFailureArea::Voice, AudioFailureState::Rejected,
                     AudioRecoveryAction::CorrectInput, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::GraphBuildFailed, AudioFailureArea::Graph, AudioFailureState::Rejected,
                     AudioRecoveryAction::RebuildCandidate, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::DeviceLost, AudioFailureArea::Device, AudioFailureState::Recovering, AudioRecoveryAction::ReopenDevice,
                     AudioActiveStatePolicy::QuiesceDevice},
                Case{&AudioErrors::ProviderFailed, AudioFailureArea::Provider, AudioFailureState::Recovering,
                     AudioRecoveryAction::RequestHostPolicy, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::MiddlewareFailed, AudioFailureArea::Middleware, AudioFailureState::Recovering,
                     AudioRecoveryAction::RequestHostPolicy, AudioActiveStatePolicy::QuiesceDevice},
                Case{&AudioErrors::MemoryAllocationFailed, AudioFailureArea::Memory, AudioFailureState::Rejected,
                     AudioRecoveryAction::RetryAtSafePoint, AudioActiveStatePolicy::Preserve},
                Case{&AudioErrors::HandleGenerationExhausted, AudioFailureArea::Runtime, AudioFailureState::Recovering,
                     AudioRecoveryAction::ReplaceRuntime, AudioActiveStatePolicy::Preserve},
            };
            for (const Case &entry : cases) {
                const auto origin = entry.active == AudioActiveStatePolicy::QuiesceDevice ? AudioFailureOrigin::ActiveEpoch
                                                                                          : AudioFailureOrigin::RequestOrCandidate;
                const auto decision = ClassifyAudioFailure(MakeError(*entry.descriptor), origin);
                CHECK(decision == AudioFailureDecision{entry.area, entry.state, entry.action, entry.active, true});
            }
        }

        TEST_CASE("Unknown and callback audio failures fail closed without fallback", "[unit][audio][failure]") {
            const Error foreign{.code = ErrorCode{"audio.device.lost"}, .domain = ErrorDomainId{"foreign.audio"}};
            const Error future{.code = ErrorCode{"audio.future.unknown"}, .domain = ErrorDomainId{"horo.audio"}};
            const AudioFailureDecision failClosed{};
            CHECK(ClassifyAudioFailure(foreign, AudioFailureOrigin::RequestOrCandidate) == failClosed);
            CHECK(ClassifyAudioFailure(future, AudioFailureOrigin::ActiveEpoch) == failClosed);
            CHECK(ClassifyAudioFailure(MakeError(AudioErrors::DeviceLost), static_cast<AudioFailureOrigin>(255)) == failClosed);

            const auto callback = ClassifyAudioFailure(MakeError(AudioErrors::StreamUnderrun), AudioFailureOrigin::CallbackFault);
            CHECK(callback.area == AudioFailureArea::Stream);
            CHECK(callback.state == AudioFailureState::Failed);
            CHECK(callback.action == AudioRecoveryAction::RequestHostPolicy);
            CHECK(callback.activeState == AudioActiveStatePolicy::RetainUntilDetached);
            CHECK(callback.recognized);
            CHECK(ClassifyAudioFailure(future, AudioFailureOrigin::CallbackFault) == failClosed);
        }

        TEST_CASE("Candidate and request failure decisions leave a playing voice unchanged", "[unit][audio][failure][voice]") {
            auto created = AudioVoiceStateMachine::Create({.owner = AudioRuntimeId::Create(61).Value(), .maximumVoices = 2});
            REQUIRE(created.HasValue());
            auto voices = std::move(created).Value();
            const auto voice = voices.CreateVoice().Value();
            REQUIRE(voices.Transition(voice, AudioVoiceState::Ready).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Scheduled).HasValue());
            REQUIRE(voices.Transition(voice, AudioVoiceState::Playing).HasValue());
            const auto before = voices.Snapshot(voice).Value();

            const auto graph = ClassifyAudioFailure(MakeError(AudioErrors::GraphStaleCandidate), AudioFailureOrigin::RequestOrCandidate);
            const auto request =
                ClassifyAudioFailure(MakeError(AudioErrors::PlaybackRequestInvalid), AudioFailureOrigin::RequestOrCandidate);
            REQUIRE(graph.activeState == AudioActiveStatePolicy::Preserve);
            REQUIRE(request.activeState == AudioActiveStatePolicy::Preserve);
            CHECK(voices.Snapshot(voice).Value() == before);

            const auto candidate = ClassifyAudioFailure(MakeError(AudioErrors::DeviceUnavailable), AudioFailureOrigin::RequestOrCandidate);
            CHECK(candidate.state == AudioFailureState::Rejected);
            CHECK(candidate.activeState == AudioActiveStatePolicy::Preserve);
            CHECK(voices.Snapshot(voice).Value() == before);

            const auto device = ClassifyAudioFailure(MakeError(AudioErrors::DeviceLost), AudioFailureOrigin::ActiveEpoch);
            CHECK(device.activeState == AudioActiveStatePolicy::QuiesceDevice);
            CHECK(voices.Snapshot(voice).Value() == before);
            REQUIRE(voices.BeginShutdown().HasValue());
            CHECK(voices.State(voice).Value() == AudioVoiceState::Cancelled);
        }

        TEST_CASE("Audio failure classification performs no allocation on control", "[unit][audio][failure]") {
            const Error error = MakeError(AudioErrors::GraphBuildFailed);
            Horo::Tests::AllocationProbe::ScopedFailure failure;
            const auto decision = ClassifyAudioFailure(error, AudioFailureOrigin::RequestOrCandidate);
            CHECK(decision.recognized);
            CHECK(decision.activeState == AudioActiveStatePolicy::Preserve);
        }
    }  // namespace
}  // namespace Horo::Audio
