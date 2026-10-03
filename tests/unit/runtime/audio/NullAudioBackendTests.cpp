#include "Horo/Audio/AudioCommandStaging.h"
#include "Horo/Audio/AudioMemory.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>

namespace Horo::Audio::Backend {
    namespace {
        AudioRuntimeId Owner(const std::uint64_t value = 71) {
            return AudioRuntimeId::Create(value).Value();
        }

        NullAudioBackendConfig Config() {
            return {.owner = Owner(), .clockDomain = 19, .clockGeneration = 3, .discontinuityRevision = 5};
        }

        AudioDeviceEpoch Epoch() {
            return {.device = {Owner(), 1, 1}, .formatRevision = 7, .callbackEpoch = 11};
        }

        AudioDeviceFormatRequest FormatRequest() {
            return {.device = Epoch().device,
                    .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                    .period = {64, 128, 256}};
        }

        Open OpenRequest() {
            return {.plannedEpoch = Epoch(), .format = FormatRequest(), .access = AccessMode::Shared};
        }

        AudioCommandStagingDescriptor StagingDescriptor() {
            return {.callback = {.owner = Owner(),
                                 .storageIdentity = AudioMemoryPoolId::Create(21).Value(),
                                 .epoch = 5,
                                 .slots = 4,
                                 .criticalSlots = 2,
                                 .budgetBytes = 4096},
                    .ingressIdentity = AudioMemoryPoolId::Create(22).Value(),
                    .ingressSlots = 4,
                    .criticalSlots = 2,
                    .sceneSlots = 4,
                    .ingressBudgetBytes = 4096};
        }

        struct RenderTrace final {
            std::array<RenderPhase, 16> phases{};
            std::array<std::uint64_t, 16> frames{};
            std::array<std::uint64_t, 16> times{};
            AudioProcessingFormat expected;
            std::size_t count{};
            bool planesAligned{true};
            bool emitSafetyHooks{};
            bool attemptOwnedOperations{};
            bool allocationSucceeded{};
            AudioCommandStagingStatus lockStatus{AudioCommandStagingStatus::Inactive};
            AudioCommandStaging *staging{};
            std::optional<AudioScratchArena> arena; /**< Retains the allocation for control-side destruction. */
        };

        RenderTrace Trace() {
            RenderTrace trace;
            trace.expected = FormatRequest().preferred;
            return trace;
        }

        RenderResult Render(void *context, const RenderInvocation &invocation) noexcept {
            auto &trace = *static_cast<RenderTrace *>(context);
            if (trace.count == trace.phases.size() || !ValidateAudioPlanarBlock(invocation.output, trace.expected))
                return {};
            trace.phases[trace.count] = invocation.phase;
            trace.frames[trace.count] = invocation.sampleFrame;
            trace.times[trace.count] = invocation.startedAt.nanoseconds;
            ++trace.count;
            if (trace.emitSafetyHooks) {
                AudioCallbackWatchdog::OnAllocationAttempt();
                AudioCallbackWatchdog::OnLockAttempt();
            }
            if (trace.attemptOwnedOperations) {
                trace.attemptOwnedOperations = false;
                auto arena = AudioScratchArena::Create(Owner(), AudioMemoryAlignment);
                trace.allocationSucceeded = arena.HasValue();
                if (arena.HasValue())
                    trace.arena.emplace(std::move(arena).Value());
                trace.lockStatus = trace.staging->RegisterScene({.owner = Owner(), .slot = 1, .generation = 1});
            }
            for (auto *plane : invocation.output.planes) {
                void *storage = plane;
                std::size_t space = 64;
                trace.planesAligned = trace.planesAligned && std::align(64, 1, storage, space) == plane;
                for (std::uint32_t frame = 0; frame < invocation.output.validFrames; ++frame)
                    plane[frame] = 0.25F;
            }
            using enum RenderPhase;
            using enum RenderDisposition;
            if (invocation.phase == Priming)
                return {.disposition = Ready, .fault = AudioCallbackFaultCode::None};
            if (invocation.phase == Quiescing)
                return {.disposition = Quiesced, .fault = AudioCallbackFaultCode::None};
            return {.disposition = Rendered, .fault = AudioCallbackFaultCode::None};
        }

        OperationId Begin(NullAudioBackend &backend, const Request &request) {
            const auto result = backend.Begin(request, {Config().clockDomain, 1'000'000'000});
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Completion Complete(NullAudioBackend &backend, const Request &request) {
            const auto operation = Begin(backend, request);
            const auto pending = backend.Poll(operation);
            REQUIRE(pending.HasValue());
            REQUIRE_FALSE(pending.Value().has_value());
            REQUIRE(backend.AdvanceControl().HasValue());
            const auto result = backend.Poll(operation);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            const auto completion = *result.Value();
            REQUIRE(backend.AcknowledgeCompletion(operation).HasValue());
            return completion;
        }

        std::unique_ptr<NullAudioBackend> Backend() {
            auto result = CreateNullAudioBackend(Config());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void OpenAndStart(NullAudioBackend &backend, RenderTrace &trace) {
            const auto enumerated = Complete(backend, Enumerate{});
            const auto &snapshot = std::get<AudioDeviceSnapshot>(enumerated.outcome);
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            REQUIRE(snapshot.devices.size() == 1);
            REQUIRE(snapshot.devices.front().deviceClass == AudioDeviceClass::Headless);

            const auto opened = Complete(backend, OpenRequest());
            const auto &facts = std::get<Opened>(opened.outcome);
            REQUIRE(ValidateAudioDeviceNegotiation(FormatRequest(), snapshot, facts.format).status ==
                    AudioDeviceNegotiationStatus::Accepted);
            REQUIRE(ValidateAudioDeviceTimingReport(facts.timing, Epoch(), AudioBackendKind::NullAudio));
            REQUIRE(facts.timing.hardwareLatency.quality == AudioObservationQuality::Unsupported);
            REQUIRE(facts.timing.endToEndLatency.quality == AudioObservationQuality::Unsupported);
            REQUIRE(backend.State() == NullAudioBackendState::Opened);

            const auto started = Complete(backend, Start{Epoch(), {&trace, Render}});
            REQUIRE(std::holds_alternative<Started>(started.outcome));
            REQUIRE(backend.State() == NullAudioBackendState::Priming);
        }

        TEST_CASE("Null audio callback safety hooks drain only on control without reading wall time",
                  "[unit][audio][null_backend][watchdog]") {
            auto backend = Backend();
            auto trace = Trace();
            trace.emitSafetyHooks = true;
            OpenAndStart(*backend, trace);
            REQUIRE(backend->AdvanceCallback().HasValue());
            std::array<AudioCallbackViolation, 4> records{};
            const auto drained = backend->DrainSafetyViolations(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 2);
            CHECK(records[0].kind == AudioCallbackViolationKind::AllocationAttempt);
            CHECK(records[1].kind == AudioCallbackViolationKind::LockAttempt);
            CHECK(records[0].epoch == Epoch());
#else
            REQUIRE(drained.count == 0);
#endif
            CHECK(backend->DrainSafetyViolations(records).count == 0);
        }

        TEST_CASE("Null callback detects actual Horo audio allocation and ingress lock attempts", "[unit][audio][null_backend][watchdog]") {
            auto stagingResult = AudioCommandStaging::Create(StagingDescriptor());
            REQUIRE(stagingResult.HasValue());
            auto staging = std::move(stagingResult).Value();
            auto backend = Backend();
            auto trace = Trace();
            trace.attemptOwnedOperations = true;
            trace.staging = &staging;
            OpenAndStart(*backend, trace);
            std::array<AudioCallbackViolation, 4> records{};
            REQUIRE(backend->DrainSafetyViolations(records).count == 0);
            REQUIRE(backend->AdvanceCallback().HasValue());
            CHECK(trace.allocationSucceeded);
            CHECK(trace.lockStatus == AudioCommandStagingStatus::Ok);
            const auto drained = backend->DrainSafetyViolations(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 2);
            CHECK(records[0].kind == AudioCallbackViolationKind::AllocationAttempt);
            CHECK(records[1].kind == AudioCallbackViolationKind::LockAttempt);
            CHECK(records[0].epoch == Epoch());
            CHECK(records[1].sampleFrame == 0);
#else
            REQUIRE(drained.count == 0);
#endif
            CHECK(backend->DrainSafetyViolations(records).count == 0);
        }

        TEST_CASE("Null audio lifecycle and sample clock are deterministic without wall time or hardware", "[unit][audio][null_backend]") {
            auto first = Backend();
            auto second = Backend();
            auto firstTrace = Trace();
            auto secondTrace = Trace();
            OpenAndStart(*first, firstTrace);
            OpenAndStart(*second, secondTrace);

            for (auto *backend : {first.get(), second.get()}) {
                const auto readyClock = backend->AdvanceCallback();
                REQUIRE(readyClock.HasValue());
                const AudioClockExpectation expectation{.owner = Owner(),
                                                        .epoch = Epoch().callbackEpoch,
                                                        .clockGeneration = Config().clockGeneration,
                                                        .discontinuityRevision = Config().discontinuityRevision,
                                                        .producerClockDomain = Config().clockDomain,
                                                        .producerGeneration = 1};
                const auto mapped =
                    MapAudioProducerTimeToSampleFrame(readyClock.Value(), expectation, readyClock.Value().producerNanoseconds);
                REQUIRE(mapped.status == AudioClockMappingStatus::Mapped);
                REQUIRE(mapped.sampleFrame == 128);
                REQUIRE(backend->AdvanceCallback().HasValue());
                REQUIRE(backend->CommitRendering(Epoch()).HasValue());
                REQUIRE(backend->AdvanceCallback().HasValue());
                REQUIRE(backend->AdvanceCallback().HasValue());
                REQUIRE(backend->SampleFrame() == 512);
            }
            REQUIRE(firstTrace.phases == secondTrace.phases);
            REQUIRE(firstTrace.frames == secondTrace.frames);
            REQUIRE(firstTrace.times == secondTrace.times);
            REQUIRE(firstTrace.count == 4);
            REQUIRE(firstTrace.planesAligned);
            REQUIRE(secondTrace.planesAligned);
            REQUIRE(firstTrace.frames[0] == 0);
            REQUIRE(firstTrace.frames[1] == 128);
            REQUIRE(firstTrace.times[1] == 2'666'666);

            std::array<Event, 4> readyEvents;
            REQUIRE(first->DrainEvents(readyEvents) == 1);
            REQUIRE(std::holds_alternative<AudioCallbackReady>(std::get<AudioCallbackEvent>(readyEvents[0].fact).fact));

            const auto clock = first->AdvanceCallback().Value();
            REQUIRE(clock.clock.sampleFrame == 640);
            REQUIRE(clock.clock.observedAt.nanoseconds == 13'333'333);
            REQUIRE(clock.clock.owner == Owner());
            REQUIRE(clock.clock.generation == 3);
            REQUIRE(clock.clock.discontinuityRevision == 5);
        }

        TEST_CASE("Null audio retains lifecycle facts and completes quiesce before detachment", "[unit][audio][null_backend]") {
            auto backend = Backend();
            auto trace = Trace();
            OpenAndStart(*backend, trace);
            REQUIRE(backend->AdvanceCallback().HasValue());
            REQUIRE(backend->CommitRendering(Epoch()).HasValue());

            const auto quiesce = Begin(*backend, Quiesce{Epoch()});
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(backend->State() == NullAudioBackendState::Quiescing);
            REQUIRE(backend->AdvanceControl().HasError());
            REQUIRE_FALSE(backend->Poll(quiesce).Value().has_value());
            REQUIRE(backend->AdvanceCallback().HasValue());
            REQUIRE(backend->State() == NullAudioBackendState::Quiesced);
            REQUIRE(std::holds_alternative<Quiesced>(backend->Poll(quiesce).Value()->outcome));
            REQUIRE(backend->AcknowledgeCompletion(quiesce).HasValue());

            REQUIRE(std::holds_alternative<Stopped>(Complete(*backend, Stop{Epoch()}).outcome));
            REQUIRE(backend->State() == NullAudioBackendState::Stopped);
            REQUIRE(std::holds_alternative<Closed>(Complete(*backend, Close{}).outcome));
            REQUIRE(backend->State() == NullAudioBackendState::Closed);

            std::array<Event, 8> events;
            const auto count = backend->DrainEvents(events);
            REQUIRE(count == 2);
            REQUIRE(std::holds_alternative<AudioCallbackReady>(std::get<AudioCallbackEvent>(events[0].fact).fact));
            REQUIRE(std::holds_alternative<AudioCallbackQuiesced>(std::get<AudioCallbackEvent>(events[1].fact).fact));
        }

        TEST_CASE("Null audio cancellation and operation slots preserve transactional state", "[unit][audio][null_backend]") {
            auto backend = Backend();
            const auto operation = Begin(*backend, Enumerate{});
            REQUIRE(backend->Begin(Probe{}, {Config().clockDomain, 1}).HasError());
            REQUIRE(backend->Cancel(operation).Value() == CancelDisposition::Requested);
            const auto completion = backend->Poll(operation).Value();
            REQUIRE(completion.has_value());
            REQUIRE(std::holds_alternative<Cancelled>(completion->outcome));
            REQUIRE(backend->State() == NullAudioBackendState::Closed);
            REQUIRE(backend->Cancel(operation).Value() == CancelDisposition::AlreadyTerminal);
            REQUIRE(backend->AcknowledgeCompletion(operation).HasValue());
            REQUIRE(backend->Poll(operation).HasError());
        }

        TEST_CASE("Null audio rejects malformed identity deadlines modes epochs and lifecycle order", "[unit][audio][null_backend]") {
            auto invalid = Config();
            invalid.clockDomain = 0;
            REQUIRE(CreateNullAudioBackend(invalid).HasError());

            auto backend = Backend();
            REQUIRE(backend->Begin(Probe{}, {}).HasError());
            auto exclusive = OpenRequest();
            exclusive.access = AccessMode::Exclusive;
            REQUIRE(backend->Begin(exclusive, {Config().clockDomain, 1}).HasError());
            REQUIRE(backend->Begin(Start{Epoch(), {}}, {Config().clockDomain, 1}).HasError());
            REQUIRE(backend->CommitRendering(Epoch()).HasError());
            REQUIRE(backend->AdvanceCallback().HasError());

            auto foreign = OpenRequest();
            foreign.plannedEpoch.device.owner = Owner(72);
            REQUIRE(backend->Begin(foreign, {Config().clockDomain, 1}).HasError());
        }

        TEST_CASE("Null audio injects typed device transitions with bounded retained delivery", "[unit][audio][null_backend]") {
            auto backend = Backend();
            auto trace = Trace();
            REQUIRE(backend->InjectDeviceLoss(DeviceLossCause::Disconnected).HasError());
            OpenAndStart(*backend, trace);
            for (std::size_t index = 0; index < 30; ++index) {
                REQUIRE(backend->InjectDeviceLoss(DeviceLossCause::ServiceRestart).HasValue());
                REQUIRE(backend->InjectInterruption(index % 2 == 0 ? InterruptionState::Began : InterruptionState::Ended).HasValue());
            }
            REQUIRE(backend->InjectDeviceLoss(DeviceLossCause::Disconnected).HasError());
            std::array<Event, 64> events;
            REQUIRE(backend->DrainEvents(events) == 60);
            REQUIRE(std::holds_alternative<DeviceLost>(events[0].fact));
            REQUIRE(std::holds_alternative<DeviceInterruption>(events[1].fact));
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
