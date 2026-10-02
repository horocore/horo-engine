#include "AudioCommandTestProbe.h"
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/AudioParameterAutomation.h"
#include "Horo/Audio/ScheduledAudioCommandBatch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    namespace {
        AudioSampleClock Clock(const std::uint64_t frame = 0) {
            return {.owner = AudioRuntimeId::Create(41).Value(),
                    .epoch = 2,
                    .generation = 3,
                    .discontinuityRevision = 4,
                    .sampleFrame = frame,
                    .sampleRate = 48'000};
        }

        AudioSceneContextHandle Scene() {
            return {.owner = Clock().owner, .slot = 1, .generation = 1};
        }

        AudioParameterAddress Address(const AudioParameterTargetKind kind = AudioParameterTargetKind::Voice) {
            AudioParameterAddress address{.kind = kind,
                                          .owner = Clock().owner,
                                          .bindingGeneration = 7,
                                          .parameter = AudioParameterId::Create(9).Value()};
            if (kind == AudioParameterTargetKind::Voice)
                address.voice = {.owner = address.owner, .slot = 2, .generation = 3};
            else
                address.bus = AudioBusId::Create(11).Value();
            if (kind == AudioParameterTargetKind::Send)
                address.send = AudioRouteId::Create(12).Value();
            if (kind == AudioParameterTargetKind::DSP)
                address.effect = AudioEffectId::Create(13).Value();
            return address;
        }

        AudioAutomationParameter Parameter(const AudioParameterAddress &address = Address()) {
            return {.address = address, .minimum = 0, .maximum = 1, .initialValue = 0, .maximumSampleDelta = 0.011F};
        }

        AudioParameterAutomationRequest Request(const std::uint64_t id = 1, const std::uint64_t start = 0, const float target = 1,
                                                const std::uint32_t duration = 100,
                                                const AudioAutomationCurve curve = AudioAutomationCurve::Linear) {
            return {.address = Address(),
                    .requestId = id,
                    .clockGeneration = 3,
                    .discontinuityRevision = 4,
                    .startFrame = start,
                    .durationFrames = duration,
                    .targetValue = target,
                    .curve = curve};
        }

        AudioCommand Command(const AudioParameterAutomationRequest &request = Request()) {
            return {.scope = {.owner = Clock().owner, .epoch = 2, .scene = Scene()}, .payload = AudioAutomateParameterCommand{request}};
        }

        void Prepare(AudioParameterAutomation &engine) {
            REQUIRE(engine.Bind(Parameter()) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Seal() == AudioAutomationStatus::Ok);
        }

        float Value(const AudioParameterAutomation &engine, const AudioParameterAddress &address = Address()) {
            float value = -1;
            REQUIRE(engine.Value(address, value) == AudioAutomationStatus::Ok);
            return value;
        }

        TEST_CASE("Automation covers stable voice bus send and DSP bindings", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            for (const auto kind : {AudioParameterTargetKind::Voice, AudioParameterTargetKind::Bus, AudioParameterTargetKind::Send,
                                    AudioParameterTargetKind::DSP}) {
                REQUIRE(engine.Bind(Parameter(Address(kind))) == AudioAutomationStatus::Ok);
            }
            REQUIRE(engine.Seal() == AudioAutomationStatus::Ok);
            std::uint64_t id = 1;
            for (const auto kind : {AudioParameterTargetKind::Voice, AudioParameterTargetKind::Bus, AudioParameterTargetKind::Send,
                                    AudioParameterTargetKind::DSP}) {
                auto request = Request(id++);
                request.address = Address(kind);
                REQUIRE(engine.Apply(Command(request)) == AudioAutomationStatus::Ok);
            }
            REQUIRE(engine.Advance(Clock(100)) == AudioAutomationStatus::Ok);
            for (const auto kind : {AudioParameterTargetKind::Voice, AudioParameterTargetKind::Bus, AudioParameterTargetKind::Send,
                                    AudioParameterTargetKind::DSP})
                REQUIRE(Value(engine, Address(kind)) == 1);
            auto oldBinding = Address();
            ++oldBinding.bindingGeneration;
            float unchanged = 5;
            REQUIRE(engine.Value(oldBinding, unchanged) == AudioAutomationStatus::MissingParameter);
            REQUIRE(unchanged == 5);
        }

        TEST_CASE("Automation overlapping ramps preserve continuity and converge to latest target", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            REQUIRE(engine.Schedule(Request()) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Schedule(Request(2, 40, 0.2F, 160, AudioAutomationCurve::Smoothstep)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Schedule(Request(3, 40, 0.8F, 100)) == AudioAutomationStatus::Ok);
            float previous = 0;
            for (std::uint64_t frame = 0; frame <= 140; ++frame) {
                REQUIRE(engine.Advance(Clock(frame)) == AudioAutomationStatus::Ok);
                const auto value = Value(engine);
                REQUIRE(std::abs(value - previous) <= 0.011001F);
                if (frame == 40)
                    REQUIRE(value == Catch::Approx(0.4F));
                previous = value;
            }
            REQUIRE(Value(engine) == 0.8F);
            REQUIRE(engine.Cancel(2) == AudioAutomationStatus::NotFound);
        }

        TEST_CASE("Automation immediate intents smooth and cubic curves meet declared sample tolerance", "[audio][automation]") {
            for (const auto curve : {AudioAutomationCurve::Immediate, AudioAutomationCurve::Smoothstep}) {
                AudioParameterAutomation engine(Clock(), Scene());
                Prepare(engine);
                const auto duration = curve == AudioAutomationCurve::Immediate ? 0U : 160U;
                REQUIRE(engine.Schedule(Request(1, 0, 1, duration, curve)) == AudioAutomationStatus::Ok);
                float previous = 0;
                for (std::uint64_t frame = 0; frame <= 160; ++frame) {
                    REQUIRE(engine.Advance(Clock(frame)) == AudioAutomationStatus::Ok);
                    const auto value = Value(engine);
                    REQUIRE(std::abs(value - previous) <= 0.011001F);
                    REQUIRE(value >= previous);
                    if (frame == 0)
                        REQUIRE(value == 0);
                    previous = value;
                }
                REQUIRE(previous == 1);
            }
        }

        TEST_CASE("Automation is invariant under callback block partition and future submission order", "[audio][automation]") {
            AudioParameterAutomation dense(Clock(), Scene()), sparse(Clock(), Scene());
            Prepare(dense);
            Prepare(sparse);
            for (auto *engine : {&dense, &sparse}) {
                REQUIRE(engine->Schedule(Request(1, 80, 0.8F, 160, AudioAutomationCurve::Smoothstep)) == AudioAutomationStatus::Ok);
                REQUIRE(engine->Schedule(Request(2, 0)) == AudioAutomationStatus::Ok);
                REQUIRE(engine->Schedule(Request(3, 40, 0.3F)) == AudioAutomationStatus::Ok);
            }
            std::uint64_t nextDenseFrame = 0;
            for (const std::uint64_t boundary : {0U, 39U, 40U, 79U, 80U, 117U, 240U}) {
                REQUIRE(sparse.Advance(Clock(boundary)) == AudioAutomationStatus::Ok);
                for (; nextDenseFrame <= boundary; ++nextDenseFrame)
                    REQUIRE(dense.Advance(Clock(nextDenseFrame)) == AudioAutomationStatus::Ok);
                REQUIRE(Value(dense) == Value(sparse));
            }
            REQUIRE(Value(sparse) == 0.8F);
        }

        TEST_CASE("Automation cancellation holds exact current value and preserves future FIFO work", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            REQUIRE(engine.Schedule(Request()) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Schedule(Request(2, 120, 0.5F)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(30)) == AudioAutomationStatus::Ok);
            auto cancel = Command();
            cancel.payload = AudioCancelAutomationCommand{.requestId = 1, .clockGeneration = 3, .discontinuityRevision = 4};
            REQUIRE(engine.Apply(cancel) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(100)) == AudioAutomationStatus::Ok);
            REQUIRE(Value(engine) == Catch::Approx(0.3F));
            REQUIRE(engine.Cancel(2) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Cancel(2) == AudioAutomationStatus::NotFound);
            REQUIRE(engine.Schedule(Request(2)) == AudioAutomationStatus::Duplicate);
            REQUIRE(engine.Schedule(Request(3, 100, 0.7F)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(200)) == AudioAutomationStatus::Ok);
            REQUIRE(Value(engine) == 0.7F);
        }

        TEST_CASE("Automation rejects stale pause late ranges curves and capacities without mutation", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            auto request = Request();
            SECTION("clock") {
                ++request.clockGeneration;
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::StaleClock);
            }
            SECTION("revision") {
                ++request.discontinuityRevision;
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::StaleClock);
            }
            SECTION("range") {
                request.targetValue = 2;
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::OutOfRange);
            }
            SECTION("nan") {
                request.targetValue = std::numeric_limits<float>::quiet_NaN();
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::InvalidInput);
            }
            SECTION("curve") {
                request.curve = static_cast<AudioAutomationCurve>(255);
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::InvalidInput);
            }
            SECTION("too short") {
                request.durationFrames = 2;
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::ContinuityLimit);
            }
            SECTION("overflow") {
                request.startFrame = std::numeric_limits<std::uint64_t>::max();
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::InvalidInput);
            }
            SECTION("binding") {
                ++request.address.bindingGeneration;
                REQUIRE(engine.Schedule(request) == AudioAutomationStatus::MissingParameter);
            }
            REQUIRE(engine.Schedule(Request()) == AudioAutomationStatus::Ok);
            auto clock = Clock(20);
            clock.state = AudioSampleClockState::Paused;
            REQUIRE(engine.Advance(clock) == AudioAutomationStatus::Paused);
            REQUIRE(Value(engine) == 0);
            clock.state = AudioSampleClockState::Running;
            ++clock.epoch;
            REQUIRE(engine.Advance(clock) == AudioAutomationStatus::StaleClock);
            REQUIRE(engine.Advance(Clock(20)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(19)) == AudioAutomationStatus::Late);
            REQUIRE(engine.Schedule(Request(2, 19)) == AudioAutomationStatus::Late);
            REQUIRE(Value(engine) == Catch::Approx(0.2F));
        }

        TEST_CASE("Automation bounded queues retain admission identity across saturation", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            for (std::uint64_t id = 1; id <= MaximumAudioAutomationRequests; ++id)
                REQUIRE(engine.Schedule(Request(id, 1000)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Schedule(Request(129, 1000)) == AudioAutomationStatus::Capacity);
            REQUIRE(engine.Cancel(64) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Schedule(Request(129, 1000, 0.7F)) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(1100)) == AudioAutomationStatus::Ok);
            REQUIRE(Value(engine) == 0.7F);
            REQUIRE(engine.Schedule(Request(130, 1100)) == AudioAutomationStatus::Ok);
        }

        TEST_CASE("Automation immutable binding preparation rejects malformed and duplicate targets", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            auto parameter = Parameter();
            SECTION("tolerance") {
                parameter.maximumSampleDelta = 0;
            }
            SECTION("range") {
                parameter.maximum = -1;
            }
            SECTION("nonfinite") {
                parameter.initialValue = std::numeric_limits<float>::infinity();
            }
            SECTION("slope overflow") {
                parameter.maximumSampleDelta = std::numeric_limits<float>::min();
            }
            SECTION("mixed address") {
                parameter.address.bus = AudioBusId::Create(8).Value();
            }
            REQUIRE(engine.Bind(parameter) == AudioAutomationStatus::InvalidInput);
            REQUIRE(engine.Bind(Parameter()) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Bind(Parameter()) == AudioAutomationStatus::Duplicate);
            for (std::uint64_t id = 2; id <= MaximumAudioAutomationParameters; ++id) {
                parameter = Parameter();
                parameter.address.parameter = AudioParameterId::Create(id + 10).Value();
                REQUIRE(engine.Bind(parameter) == AudioAutomationStatus::Ok);
            }
            parameter.address.parameter = AudioParameterId::Create(999).Value();
            REQUIRE(engine.Bind(parameter) == AudioAutomationStatus::Capacity);
            REQUIRE(engine.Seal() == AudioAutomationStatus::Ok);
            REQUIRE(engine.Bind(parameter) == AudioAutomationStatus::Closed);
        }

        TEST_CASE("Automation actually consumes normal SPSC and scheduled batch commands allocation free", "[audio][automation]") {
            auto created = AudioCommandBuffer::Create({.owner = Clock().owner,
                                                       .storageIdentity = AudioMemoryPoolId::Create(1).Value(),
                                                       .epoch = 2,
                                                       .slots = 8,
                                                       .criticalSlots = 1,
                                                       .budgetBytes = 64 * 1024});
            REQUIRE(created.HasValue());
            auto buffer = std::move(created).Value();
            ScheduledAudioCommandBatch input{.target = {.kind = AudioCommandTargetKind::ExactSampleFrame,
                                                        .sampleFrame = 0,
                                                        .clockGeneration = 3,
                                                        .discontinuityRevision = 4},
                                             .commandCount = 2};
            input.commands[0] = Command();
            input.commands[1] = Command(Request(2, 0, 0.5F));
            ScheduledAudioCommandBatch normalized;
            REQUIRE(NormalizeScheduledAudioCommandBatch(input, normalized) == ScheduledAudioCommandBatchStatus::Ok);
            REQUIRE(buffer.TryPublish({.sequence = 1, .command = normalized.commands[0]}) == AudioCommandPublishStatus::Published);
            REQUIRE(buffer.TryPublish({.sequence = 2, .command = normalized.commands[1]}) == AudioCommandPublishStatus::Published);
            REQUIRE_FALSE(CanCoalesceAudioCommands(input.commands[0], input.commands[1]));
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            AudioCommandRecord record;
            const auto allocations = Test::allocationCount;
            const auto deallocations = Test::deallocationCount;
            const bool first = buffer.TryConsume(record);
            const auto firstStatus = engine.Apply(record.command);
            const bool second = buffer.TryConsume(record);
            const auto secondStatus = engine.Apply(record.command);
            const auto advanced = engine.Advance(Clock(100));
            const auto finalAllocations = Test::allocationCount;
            const auto finalDeallocations = Test::deallocationCount;
            REQUIRE(first);
            REQUIRE(second);
            REQUIRE(firstStatus == AudioAutomationStatus::Ok);
            REQUIRE(secondStatus == AudioAutomationStatus::Ok);
            REQUIRE(advanced == AudioAutomationStatus::Ok);
            REQUIRE(finalAllocations == allocations);
            REQUIRE(finalDeallocations == deallocations);
            REQUIRE(Value(engine) == 0.5F);
        }

        TEST_CASE("Automation command validation fences cancellation and foreign scenes", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            auto command = Command();
            SECTION("foreign scene") {
                ++command.scope.scene.generation;
                REQUIRE(engine.Apply(command) == AudioAutomationStatus::StaleClock);
            }
            SECTION("foreign epoch") {
                ++command.scope.epoch;
                REQUIRE(engine.Apply(command) == AudioAutomationStatus::StaleClock);
            }
            SECTION("invalid parameter") {
                std::get<AudioAutomateParameterCommand>(command.payload).request.address.parameter = {};
                REQUIRE(engine.Apply(command) == AudioAutomationStatus::InvalidInput);
            }
            SECTION("stale cancel") {
                command.payload = AudioCancelAutomationCommand{.requestId = 1, .clockGeneration = 2, .discontinuityRevision = 4};
                REQUIRE(engine.Apply(command) == AudioAutomationStatus::StaleClock);
            }
            REQUIRE(engine.Apply(Command()) == AudioAutomationStatus::Ok);
        }

        TEST_CASE("Automation unload and reset barriers close pending work", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            REQUIRE(engine.Apply(Command()) == AudioAutomationStatus::Ok);
            auto command = Command();
            SECTION("unload") {
                command.payload = AudioSceneUnloadCommand{};
            }
            SECTION("reset") {
                command.payload = AudioResetCommand{};
                command.scope.scene = {};
            }
            REQUIRE(engine.Apply(command) == AudioAutomationStatus::Ok);
            REQUIRE(engine.Advance(Clock(100)) == AudioAutomationStatus::Closed);
            REQUIRE(engine.Cancel(1) == AudioAutomationStatus::Closed);
            REQUIRE(engine.Schedule(Request(2)) == AudioAutomationStatus::Closed);
            REQUIRE(engine.Seal() == AudioAutomationStatus::Closed);
            engine.Close();
        }

        TEST_CASE("Automation preparation and payload shape reject invalid authority", "[audio][automation]") {
            auto clock = Clock();
            SECTION("rate") {
                clock.sampleRate = 0;
            }
            SECTION("paused") {
                clock.state = AudioSampleClockState::Paused;
            }
            SECTION("empty owner") {
                clock.owner = {};
            }
            SECTION("generation") {
                clock.generation = 0;
            }
            AudioParameterAutomation invalid(clock, Scene());
            REQUIRE(invalid.Seal() == AudioAutomationStatus::Closed);
            REQUIRE(invalid.Bind(Parameter()) == AudioAutomationStatus::Closed);
            REQUIRE(invalid.Apply(Command()) == AudioAutomationStatus::Closed);
            float value = 99;
            REQUIRE(invalid.Value(Address(), value) == AudioAutomationStatus::Closed);
            REQUIRE(value == 99);
            AudioParameterAutomation unsealed(Clock(), Scene());
            REQUIRE(unsealed.Schedule(Request()) == AudioAutomationStatus::InvalidInput);
            REQUIRE(unsealed.Advance(Clock()) == AudioAutomationStatus::InvalidInput);
            auto foreign = Parameter();
            foreign.address.owner = AudioRuntimeId::Create(999).Value();
            foreign.address.voice.owner = foreign.address.owner;
            REQUIRE(unsealed.Bind(foreign) == AudioAutomationStatus::InvalidInput);
        }

        TEST_CASE("Automation target shapes never reinterpret malformed addresses", "[audio][automation]") {
            auto address = Address(AudioParameterTargetKind::Bus);
            REQUIRE(IsValidAudioParameterAddress(address));
            address.send = AudioRouteId::Create(1).Value();
            REQUIRE_FALSE(IsValidAudioParameterAddress(address));
            address = Address(AudioParameterTargetKind::Send);
            address.send = {};
            REQUIRE_FALSE(IsValidAudioParameterAddress(address));
            address = Address(AudioParameterTargetKind::DSP);
            address.effect = {};
            REQUIRE_FALSE(IsValidAudioParameterAddress(address));
            address = Address(AudioParameterTargetKind::Bus);
            address.kind = static_cast<AudioParameterTargetKind>(255);
            REQUIRE_FALSE(IsValidAudioParameterAddress(address));
            auto request = Request();
            request.curve = AudioAutomationCurve::Immediate;
            REQUIRE_FALSE(IsValidAudioAutomationRequest(request));
            request.durationFrames = 0;
            REQUIRE(IsValidAudioAutomationRequest(request));
            request.requestId = 0;
            REQUIRE_FALSE(IsValidAudioAutomationRequest(request));
            auto command = Command();
            command.payload = AudioCancelAutomationCommand{};
            AudioCommand normalized;
            REQUIRE(NormalizeAudioCommand(command, normalized) == AudioCommandStatus::InvalidPayload);
        }

        TEST_CASE("Automation retained scheduled references traverse SPSC and honor closing barriers", "[audio][automation]") {
            auto created = AudioCommandBuffer::Create({.owner = Clock().owner,
                                                       .storageIdentity = AudioMemoryPoolId::Create(3).Value(),
                                                       .epoch = 2,
                                                       .slots = 4,
                                                       .criticalSlots = 1,
                                                       .budgetBytes = 64 * 1024});
            REQUIRE(created.HasValue());
            auto buffer = std::move(created).Value();
            ScheduledAudioCommandBatch retained{.target = {.kind = AudioCommandTargetKind::ExactSampleFrame,
                                                           .sampleFrame = 0,
                                                           .clockGeneration = 3,
                                                           .discontinuityRevision = 4},
                                                .commandCount = 2};
            retained.commands[0] = Command();
            retained.commands[1] = Command(Request(2, 0, 0.7F));
            const AudioMemoryHandle storage{.owner = Clock().owner,
                                            .pool = AudioMemoryPoolId::Create(5).Value(),
                                            .slot = 1,
                                            .generation = 8};
            AudioCommand publication;
            REQUIRE(MakeScheduledAudioBatchCommand(retained, storage, publication) == ScheduledAudioCommandBatchStatus::Ok);
            REQUIRE(buffer.TryPublish({.sequence = 1, .command = publication}) == AudioCommandPublishStatus::Published);
            auto barrier = Command();
            barrier.payload = AudioSceneUnloadCommand{};
            REQUIRE(buffer.TryPublish({.sequence = 2, .command = barrier}) == AudioCommandPublishStatus::Published);
            buffer.Close();
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            AudioCommandRecord consumed;
            REQUIRE(buffer.TryConsume(consumed));
            const auto reference = std::get<AudioScheduledBatchCommand>(consumed.command.payload);
            REQUIRE(reference.storage == storage);
            REQUIRE(reference.commandCount == retained.commandCount);
            const auto allocationCount = Test::allocationCount;
            const auto applied = engine.ApplyBatch(retained);
            const auto advanced = engine.Advance(Clock(100));
            const auto after = Test::allocationCount;
            REQUIRE(applied == AudioAutomationStatus::Ok);
            REQUIRE(advanced == AudioAutomationStatus::Ok);
            REQUIRE(after == allocationCount);
            REQUIRE(Value(engine) == 0.7F);
            REQUIRE(buffer.TryConsume(consumed));
            REQUIRE(engine.Apply(consumed.command) == AudioAutomationStatus::Ok);
            REQUIRE(buffer.IsDrained());
            REQUIRE(engine.ApplyBatch(retained) == AudioAutomationStatus::Closed);
        }

        TEST_CASE("Automation retained batches apply atomically without partial request admission", "[audio][automation]") {
            AudioParameterAutomation engine(Clock(), Scene());
            Prepare(engine);
            ScheduledAudioCommandBatch batch{.target = {.kind = AudioCommandTargetKind::ExactSampleFrame,
                                                        .sampleFrame = 0,
                                                        .clockGeneration = 3,
                                                        .discontinuityRevision = 4},
                                             .commandCount = 2};
            batch.commands[0] = Command();
            batch.commands[1] = Command(Request(2, 0, 2));
            REQUIRE(engine.ApplyBatch(batch) == AudioAutomationStatus::OutOfRange);
            REQUIRE(Value(engine) == 0);
            batch.commands[1] = Command(Request(2, 0, 0.5F));
            const auto allocations = Test::allocationCount;
            const auto status = engine.ApplyBatch(batch);
            const auto after = Test::allocationCount;
            REQUIRE(status == AudioAutomationStatus::Ok);
            REQUIRE(after == allocations);
            REQUIRE(engine.Advance(Clock(100)) == AudioAutomationStatus::Ok);
            REQUIRE(Value(engine) == 0.5F);
            batch.target.sampleFrame = 99;
            REQUIRE(engine.ApplyBatch(batch) == AudioAutomationStatus::StaleClock);
            batch.target.sampleFrame = 100;
            batch.commands[0] = Command(Request(3, 100));
            batch.commands[1] = Command(Request(4, 100));
            batch.commands[1].payload = AudioStartVoiceCommand{Address().voice};
            REQUIRE(engine.ApplyBatch(batch) == AudioAutomationStatus::InvalidInput);
            REQUIRE(engine.Schedule(Request(3, 100)) == AudioAutomationStatus::Ok);
        }
    }  // namespace
}  // namespace Horo::Audio
