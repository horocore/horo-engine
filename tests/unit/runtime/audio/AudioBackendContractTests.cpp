#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/AudioBackend.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::Audio::Backend {
    namespace {
        AudioDeviceEpoch Epoch() {
            return {.device = {AudioRuntimeId::Create(7).Value(), 1, 2}, .formatRevision = 3, .callbackEpoch = 4};
        }

        Open OpenRequest() {
            return {.plannedEpoch = Epoch(),
                    .format = {.device = Epoch().device,
                               .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                               .period = {64, 128, 256}},
                    .access = AccessMode::Exclusive,
                    .latency = LatencyClass::Interactive};
        }

        TEST_CASE("Audio internal backend exposes an asynchronous typed control interface", "[audio][backend]") {
            static_assert(std::is_abstract_v<AudioBackend>);
            static_assert(std::has_virtual_destructor_v<AudioBackend>);
            static_assert(std::is_same_v<decltype(&AudioBackend::Begin),
                                         Result<OperationId> (AudioBackend::*)(const Request &, const AudioMonotonicTimestamp &)>);
            static_assert(
                std::is_same_v<decltype(&AudioBackend::Poll), Result<std::optional<Completion>> (AudioBackend::*)(const OperationId &)>);
            static_assert(std::is_same_v<decltype(&AudioBackend::DrainEvents), std::size_t (AudioBackend::*)(std::span<Event>) noexcept>);
            REQUIRE(std::variant_size_v<Request> == 7);
        }

        TEST_CASE("Audio backend open policy owns complete requested format values", "[audio][backend]") {
            auto source = OpenRequest();
            source.format.allowedAlternatives.push_back({44'100, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)});
            const Request retained = source;
            source.format.preferred.layout.orderedChannels.clear();
            source.format.allowedAlternatives.clear();
            const auto &copy = std::get<Open>(retained);
            REQUIRE(ValidateAudioDeviceFormatRequest(copy.format));
            REQUIRE(copy.format.preferred.layout.orderedChannels.size() == 2);
            REQUIRE(copy.format.allowedAlternatives.size() == 1);
            REQUIRE(copy.access == AccessMode::Exclusive);
            REQUIRE(copy.latency == LatencyClass::Interactive);
            REQUIRE(MatchesAudioDeviceEpoch(copy.plannedEpoch, Epoch()));
        }

        TEST_CASE("Audio backend requested and actual facts remain distinct for every peer", "[audio][backend]") {
            auto request = OpenRequest();
            request.access = AccessMode::Shared;
            request.format.allowedAlternatives.push_back({44'100, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)});
            for (const auto kind : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire,
                                    AudioBackendKind::SDL3Audio, AudioBackendKind::NullAudio}) {
                const AudioBackendProbe probe{.backend = kind};
                const Completion probeResult{.operation = {Epoch().device.owner, 1}, .outcome = probe};
                REQUIRE(std::get<AudioBackendProbe>(probeResult.outcome).backend == kind);
                const Opened opened{.format = {.device = Epoch().device,
                                               .discoveryRevision = 1,
                                               .formatRevision = 3,
                                               .effective = {44'100, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)},
                                               .nativeSignal = {44'100, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)},
                                               .nativeChannelForHoro = {0},
                                               .callbackFrames = 256,
                                               .deviations = {.sampleRate = AudioFormatDeviationReason::DeviceConstraint,
                                                              .layout = AudioFormatDeviationReason::HostPolicy,
                                                              .callbackPeriod = AudioFormatDeviationReason::DeviceConstraint}},
                                    .timing = {.epoch = Epoch(), .capturedAt = {1, 100}},
                                    .access = AccessMode::Shared};
                const Completion openedResult{.operation = {Epoch().device.owner, 2}, .outcome = opened};
                REQUIRE(std::get<Opened>(openedResult.outcome).format.callbackFrames == 256);
                REQUIRE(std::get<Opened>(openedResult.outcome).format.effective.sampleRate == 44'100);
                REQUIRE(request.format.preferred.sampleRate == 48'000);
                REQUIRE(request.format.period.preferredFrames == 128);
            }
        }

        TEST_CASE("Audio backend terminal lifecycle outcomes do not conflate readiness and detachment", "[audio][backend]") {
            const OperationId operation{Epoch().device.owner, 10};
            const std::array<Completion, 4> completions{Completion{operation, Started{Epoch()}}, Completion{operation, Quiesced{Epoch()}},
                                                        Completion{operation, Stopped{Epoch()}}, Completion{operation, Closed{}}};
            REQUIRE(std::holds_alternative<Started>(completions[0].outcome));
            REQUIRE(std::holds_alternative<Quiesced>(completions[1].outcome));
            REQUIRE(std::holds_alternative<Stopped>(completions[2].outcome));
            REQUIRE(std::holds_alternative<Closed>(completions[3].outcome));
            for (const auto &completion : completions)
                REQUIRE(completion.operation == operation);
            static_assert(!std::is_convertible_v<Quiesced, Stopped>);
        }

        TEST_CASE("Audio backend query failure pending and terminal operation failure remain distinct", "[audio][backend]") {
            using Progress = Result<std::optional<Completion>>;
            const auto pending = Progress::Success(std::nullopt);
            REQUIRE(pending.HasValue());
            REQUIRE_FALSE(pending.Value());
            const auto failure = MakeError(AudioErrors::BackendFailed, "native stop did not prove detachment");
            const Completion completion{{Epoch().device.owner, 9}, Failed{failure, ResourceDisposition::Retained}};
            const auto terminal = Progress::Success(completion);
            REQUIRE(terminal.HasValue());
            REQUIRE(terminal.Value().has_value());
            const auto copy = *terminal.Value();
            const auto &failed = std::get<Failed>(copy.outcome);
            REQUIRE(failed.resources == ResourceDisposition::Retained);
            REQUIRE(failed.error.code.Value() == AudioErrors::BackendFailed.code.Value());
            REQUIRE(failed.error.message == failure.message);
            const auto invalidQuery = Progress::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
            REQUIRE(invalidQuery.HasError());
            REQUIRE(invalidQuery.ErrorValue().code.Value() == AudioErrors::HandleOwnerMismatch.code.Value());
        }

        TEST_CASE("Audio backend cancellation carries explicit resource disposition", "[audio][backend]") {
            const OperationId operation{Epoch().device.owner, 5};
            for (const auto disposition :
                 {ResourceDisposition::Unchanged, ResourceDisposition::Closed, ResourceDisposition::CallbackDetached}) {
                const Completion terminal{operation, Cancelled{disposition}};
                REQUIRE(std::get<Cancelled>(terminal.outcome).resources == disposition);
                REQUIRE_FALSE(std::holds_alternative<Closed>(terminal.outcome));
            }
            REQUIRE(CancelDisposition::Requested != CancelDisposition::AlreadyTerminal);
            REQUIRE(OperationId{} != operation);
            REQUIRE((OperationId{AudioRuntimeId::Create(8).Value(), 5} != operation));
        }

        TEST_CASE("Audio backend event values retain original generation and critical callback facts", "[audio][backend]") {
            const AudioCallbackEvent callback{.epoch = Epoch(), .sampleFrame = 20, .timestamp = {1, 30}, .fact = AudioCallbackQuiesced{}};
            const Event source{Epoch().device.owner, callback};
            const auto copy = source;
            auto replacement = Epoch();
            ++replacement.callbackEpoch;
            const auto &fact = std::get<AudioCallbackEvent>(copy.fact);
            REQUIRE(fact.epoch.callbackEpoch == 4);
            REQUIRE_FALSE(ValidateAudioCallbackEvent(fact, replacement, 1));
            REQUIRE(IsCriticalAudioCallbackFact(fact.fact));
            static_assert(std::is_trivially_copyable_v<Event>);
            static_assert(sizeof(Event) <= 160);
        }

        TEST_CASE("Audio backend catalog loss and interruption events need no native identifiers", "[audio][backend]") {
            const auto owner = Epoch().device.owner;
            const std::array<Event, 4> events{Event{owner, DevicesChanged{2}},
                                              Event{owner, DefaultChanged{3, AudioDefaultDeviceRole::Console, std::nullopt}},
                                              Event{owner, DeviceLost{Epoch(), DeviceLossCause::ServiceRestart}},
                                              Event{owner, DeviceInterruption{Epoch(), InterruptionState::Began}}};
            REQUIRE(std::get<DevicesChanged>(events[0].fact).discoveryRevision == 2);
            REQUIRE_FALSE(std::get<DefaultChanged>(events[1].fact).device);
            REQUIRE(std::get<DeviceLost>(events[2].fact).epoch == Epoch());
            REQUIRE(std::get<DeviceInterruption>(events[3].fact).state == InterruptionState::Began);
            REQUIRE(std::get<DeviceInterruption>(events[3].fact).epoch == Epoch());
        }

        TEST_CASE("Audio render port is borrowed noexcept and fails safely before installation", "[audio][backend]") {
            const RenderPort empty;
            REQUIRE(empty.context == nullptr);
            REQUIRE(empty.process == nullptr);
            REQUIRE(RenderResult{}.disposition == RenderDisposition::Fault);
            REQUIRE(RenderResult{}.fault == AudioCallbackFaultCode::InvalidEpoch);
            static_assert(std::is_trivially_copyable_v<RenderInvocation>);
            static_assert(std::is_trivially_copyable_v<RenderResult>);
            static_assert(std::is_trivially_copyable_v<RenderPort>);
            static_assert(std::is_nothrow_invocable_r_v<RenderResult, decltype(RenderPort::process), void *, const RenderInvocation &>);
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
