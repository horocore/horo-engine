#include "AllocationProbe.h"
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/CoreAudioDSPNode.h"
#include "Horo/Audio/MixerSnapshot.h"

#include <algorithm>
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

        AudioCommandScope Scope() {
            return {.owner = Clock().owner, .epoch = 2, .scene = {.owner = Clock().owner, .slot = 1, .generation = 1}};
        }

        AudioCommandTarget Target(const std::uint64_t frame = 0) {
            return {.kind = AudioCommandTargetKind::ExactSampleFrame,
                    .sampleFrame = frame,
                    .clockGeneration = 3,
                    .discontinuityRevision = 4};
        }

        struct Fixture {
            MixerSnapshotAsset asset;
            std::array<AudioAutomationParameter, 3> bindings;
            AudioParameterAutomation automation{Clock(), Scope().scene};
            MixerSnapshotTransitions transitions{automation};

            Fixture() {
                std::array<std::uint8_t, 16> identity{};
                identity[0] = 9;
                asset.mixer = Assets::AssetId::FromBytes(identity);
                std::copy_n("pause_menu", 10, asset.name.begin());
                asset.parameterCount = 3;
                for (std::size_t index = 0; index < 3; ++index) {
                    auto &parameter = asset.parameters[index];
                    parameter.kind = static_cast<AudioParameterTargetKind>(index + 1);
                    parameter.bus = AudioBusId::Create(11).Value();
                    parameter.parameter = AudioParameterId::Create(index + 1).Value();
                    parameter.value = 1.0F;
                    if (parameter.kind == AudioParameterTargetKind::Send)
                        parameter.send = AudioRouteId::Create(12).Value();
                    if (parameter.kind == AudioParameterTargetKind::DSP)
                        parameter.effect = AudioEffectId::Create(13).Value();
                    bindings[index] = {.address = {.kind = parameter.kind,
                                                   .owner = Clock().owner,
                                                   .bus = parameter.bus,
                                                   .send = parameter.send,
                                                   .effect = parameter.effect,
                                                   .bindingGeneration = 7,
                                                   .parameter = parameter.parameter},
                                       .minimum = 0,
                                       .maximum = 1,
                                       .initialValue = 0,
                                       .maximumSampleDelta = 0.011F};
                    REQUIRE(automation.Bind(bindings[index]) == AudioAutomationStatus::Ok);
                }
                REQUIRE(automation.Seal() == AudioAutomationStatus::Ok);
            }

            PreparedMixerSnapshot Prepare(const std::uint64_t transition = 1, const std::uint64_t request = 1,
                                          const std::uint64_t frame = 0, const std::int32_t priority = 0,
                                          const AudioAutomationCurve curve = AudioAutomationCurve::Linear) {
                PreparedMixerSnapshot prepared;
                REQUIRE(PrepareMixerSnapshot(asset, asset.mixer, bindings,
                                             {Scope(), Target(frame), transition, request, priority,
                                              curve == AudioAutomationCurve::Immediate ? 0U : 160U, curve},
                                             prepared) == MixerSnapshotStatus::Ok);
                return prepared;
            }

            float Value(const std::size_t index = 0) {
                float value = -1;
                REQUIRE(automation.Value(bindings[index].address, value) == AudioAutomationStatus::Ok);
                return value;
            }
        };

        /** @brief Verify retained batch transport before callback allocation accounting begins. */
        void VerifyTransport(const PreparedMixerSnapshot &prepared) {
            const AudioMemoryHandle storage{.owner = Clock().owner,
                                            .pool = AudioMemoryPoolId::Create(5).Value(),
                                            .slot = 1,
                                            .generation = 8};
            auto created = AudioCommandBuffer::Create({.owner = Clock().owner,
                                                       .storageIdentity = AudioMemoryPoolId::Create(1).Value(),
                                                       .epoch = 2,
                                                       .slots = 8,
                                                       .criticalSlots = 1,
                                                       .budgetBytes = 64 * 1024});
            REQUIRE(created.HasValue());
            auto buffer = std::move(created).Value();
            AudioCommand command;
            REQUIRE(MakeScheduledAudioBatchCommand(prepared.batch, storage, command) == ScheduledAudioCommandBatchStatus::Ok);
            REQUIRE(buffer.TryPublish({.sequence = 1, .command = command}) == AudioCommandPublishStatus::Published);
            AudioCommandRecord consumed;
            REQUIRE(buffer.TryConsume(consumed));
            const auto reference = std::get<AudioScheduledBatchCommand>(consumed.command.payload);
            REQUIRE(reference.storage == storage);
            REQUIRE(reference.commandCount == prepared.batch.commandCount);
            REQUIRE_FALSE(buffer.TryConsume(consumed));
        }
    }  // namespace

    TEST_CASE("Mixer snapshot serialization retains named stable references and excludes runtime editor state",
              "[audio][snapshot][schema]") {
        Fixture fixture;
        std::array<std::byte, MaximumMixerSnapshotSerializedBytes> bytes{};
        std::size_t written{};
        REQUIRE(SerializeMixerSnapshot(fixture.asset, bytes, written) == MixerSnapshotStatus::Ok);
        MixerSnapshotAsset decoded;
        REQUIRE(DeserializeMixerSnapshot(std::span{bytes}.first(written), decoded) == MixerSnapshotStatus::Ok);
        CHECK(decoded == fixture.asset);
        std::array<std::byte, MaximumMixerSnapshotSerializedBytes> again{};
        std::size_t rewritten{};
        REQUIRE(SerializeMixerSnapshot(decoded, again, rewritten) == MixerSnapshotStatus::Ok);
        CHECK(bytes == again);
        CHECK(written == rewritten);
        const auto before = decoded;
        for (std::size_t size = 0; size < written; ++size) {
            CHECK(DeserializeMixerSnapshot(std::span{bytes}.first(size), decoded) != MixerSnapshotStatus::Ok);
            CHECK(decoded == before);
        }
        CHECK(DeserializeMixerSnapshot(std::span{bytes}.first(written + 1), decoded) == MixerSnapshotStatus::InvalidSerialization);
        bytes[4] = std::byte{2};
        CHECK(DeserializeMixerSnapshot(std::span{bytes}.first(written), decoded) == MixerSnapshotStatus::UnsupportedVersion);
        CHECK(decoded == before);
        written = 17;
        CHECK(SerializeMixerSnapshot(fixture.asset, std::span{bytes}.first(1), written) == MixerSnapshotStatus::Capacity);
        CHECK(written == 17);
    }

    TEST_CASE("Mixer snapshot schema rejects malformed duplicate oversized and voice targets", "[audio][snapshot][schema]") {
        Fixture fixture;
        SECTION("duplicate") {
            fixture.asset.parameters[1] = fixture.asset.parameters[0];
        }
        SECTION("voice") {
            fixture.asset.parameters[0].kind = AudioParameterTargetKind::Voice;
        }
        SECTION("oversized") {
            fixture.asset.parameterCount = MaximumMixerSnapshotParameters + 1;
        }
        SECTION("nonfinite") {
            fixture.asset.parameters[0].value = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("empty") {
            fixture.asset.parameterCount = 0;
        }
        SECTION("missing mixer") {
            fixture.asset.mixer = {};
        }
        SECTION("unknown kind") {
            fixture.asset.parameters[0].kind = static_cast<AudioParameterTargetKind>(255);
        }
        SECTION("unterminated") {
            fixture.asset.name.fill('x');
        }
        SECTION("hidden reference") {
            fixture.asset.parameters[7] = fixture.asset.parameters[0];
        }
        CHECK(ValidateMixerSnapshot(fixture.asset) == MixerSnapshotStatus::InvalidAsset);
    }

    TEST_CASE("Mixer snapshot binding resolution rejects ambiguity and preserves candidate", "[audio][snapshot][bindings]") {
        Fixture fixture;
        const auto before = fixture.Prepare();
        auto candidate = before;
        auto bindings = fixture.bindings;
        SECTION("foreign owner") {
            bindings[0].address.owner = AudioRuntimeId::Create(42).Value();
        }
        SECTION("missing") {
            bindings[0].address.parameter = AudioParameterId::Create(99).Value();
        }
        SECTION("invalid physical shape") {
            bindings[0].address.bindingGeneration = 0;
        }
        SECTION("range") {
            bindings[0].maximum = 0.5F;
        }
        CHECK(PrepareMixerSnapshot(fixture.asset, fixture.asset.mixer, bindings,
                                   {Scope(), Target(), 2, 4, 0, 160, AudioAutomationCurve::Linear},
                                   candidate) == MixerSnapshotStatus::InvalidBinding);
        CHECK(candidate.transitionId == before.transitionId);
        CHECK(candidate.batch.commands[0].scope == before.batch.commands[0].scope);
        auto ambiguous = std::array{fixture.bindings[0], fixture.bindings[0], fixture.bindings[1], fixture.bindings[2]};
        CHECK(PrepareMixerSnapshot(fixture.asset, fixture.asset.mixer, ambiguous,
                                   {Scope(), Target(), 2, 4, 0, 160, AudioAutomationCurve::Linear},
                                   candidate) == MixerSnapshotStatus::InvalidBinding);
    }

    TEST_CASE("Mixer snapshot mixed scopes and duplicate request identities preserve atomic admission", "[audio][snapshot][atomic]") {
        Fixture fixture;
        auto prepared = fixture.Prepare();
        SECTION("mixed scope") {
            ++prepared.batch.commands[1].scope.epoch;
            CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::InvalidTransition);
        }
        SECTION("duplicate request across distinct targets") {
            std::get<AudioAutomateParameterCommand>(prepared.batch.commands[1].payload).request.requestId = 1;
            CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::AutomationRejected);
            CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::Duplicate);
        }
        for (std::size_t index = 0; index < 3; ++index)
            CHECK(fixture.Value(index) == 0);
        REQUIRE(fixture.transitions.Apply(fixture.Prepare()) == MixerSnapshotStatus::Ok);
    }

    TEST_CASE("Mixer snapshot failed admission never starts a partial bus send or effect change", "[audio][snapshot][atomic]") {
        Fixture fixture;
        auto prepared = fixture.Prepare();
        std::get<AudioAutomateParameterCommand>(prepared.batch.commands[2].payload).request.targetValue = 2;
        CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::AutomationRejected);
        CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::OutOfRange);
        REQUIRE(fixture.automation.Advance(Clock(80)) == AudioAutomationStatus::Ok);
        for (std::size_t index = 0; index < 3; ++index)
            CHECK(fixture.Value(index) == 0);
        prepared = fixture.Prepare(1, 1, 80);
        REQUIRE(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::Ok);
        REQUIRE(fixture.automation.Advance(Clock(240)) == AudioAutomationStatus::Ok);
        for (std::size_t index = 0; index < 3; ++index)
            CHECK(fixture.Value(index) == 1);
    }

    TEST_CASE("Mixer snapshot precedence replacement and cancellation hold exact current trajectories", "[audio][snapshot][precedence]") {
        Fixture fixture;
        REQUIRE(fixture.transitions.Apply(fixture.Prepare(1, 1, 0, 5)) == MixerSnapshotStatus::Ok);
        REQUIRE(fixture.automation.Advance(Clock(80)) == AudioAutomationStatus::Ok);
        CHECK(fixture.Value() == Catch::Approx(0.5F));
        fixture.asset.parameters[0].value = 0;
        const auto lower = fixture.Prepare(2, 4, 80, 4);
        CHECK(fixture.transitions.Apply(lower) == MixerSnapshotStatus::Precedence);
        auto rejected = fixture.Prepare(2, 4, 80, 5);
        std::get<AudioAutomateParameterCommand>(rejected.batch.commands[2].payload).request.targetValue = 2;
        CHECK(fixture.transitions.Apply(rejected) == MixerSnapshotStatus::AutomationRejected);
        REQUIRE(fixture.automation.Advance(Clock(81)) == AudioAutomationStatus::Ok);
        CHECK(fixture.Value() == Catch::Approx(81.0F / 160.0F));
        REQUIRE(fixture.transitions.Apply(fixture.Prepare(2, 4, 81, 5)) == MixerSnapshotStatus::Ok);
        CHECK(fixture.Value() == Catch::Approx(81.0F / 160.0F));
        CHECK_FALSE(fixture.automation.HasRequest(1));
        CHECK(fixture.transitions.Cancel(1, Target(81)) == MixerSnapshotStatus::NotFound);
        REQUIRE(fixture.automation.Advance(Clock(121)) == AudioAutomationStatus::Ok);
        const auto held = fixture.Value();
        REQUIRE(fixture.transitions.Cancel(2, Target(121)) == MixerSnapshotStatus::Ok);
        REQUIRE(fixture.automation.Advance(Clock(300)) == AudioAutomationStatus::Ok);
        CHECK(fixture.Value() == held);
        CHECK(fixture.transitions.Apply(fixture.Prepare(2, 7, 300)) == MixerSnapshotStatus::Duplicate);
        CHECK(fixture.transitions.Apply(fixture.Prepare(3, 7, 300, -10)) == MixerSnapshotStatus::Ok);
    }

    TEST_CASE("Mixer snapshots fence future clock generations closed engines and capacity", "[audio][snapshot][lifecycle]") {
        Fixture fixture;
        auto prepared = fixture.Prepare(1, 1, 100);
        CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::AutomationRejected);
        CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::StaleClock);
        REQUIRE(fixture.automation.Advance(Clock(100)) == AudioAutomationStatus::Ok);
        REQUIRE(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::Ok);
        auto stale = Target(100);
        ++stale.discontinuityRevision;
        CHECK(fixture.transitions.Cancel(1, stale) == MixerSnapshotStatus::AutomationRejected);
        CHECK(fixture.automation.HasRequest(1));
        fixture.automation.Close();
        CHECK(fixture.transitions.Apply(fixture.Prepare(2, 4, 100)) == MixerSnapshotStatus::AutomationRejected);
        CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::Closed);
    }

    TEST_CASE("Mixer snapshot saturation retains all queued identities and permits exact retry", "[audio][snapshot][capacity]") {
        Fixture fixture;
        for (std::uint64_t id = 1; id <= MaximumAudioAutomationRequests; ++id) {
            AudioParameterAutomationRequest request{.address = fixture.bindings[0].address,
                                                    .requestId = id,
                                                    .clockGeneration = 3,
                                                    .discontinuityRevision = 4,
                                                    .startFrame = 1000,
                                                    .durationFrames = 160,
                                                    .targetValue = 1,
                                                    .curve = AudioAutomationCurve::Linear};
            REQUIRE(fixture.automation.Schedule(request) == AudioAutomationStatus::Ok);
        }
        const auto prepared = fixture.Prepare(1, 129);
        CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::AutomationRejected);
        CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::Capacity);
        CHECK_FALSE(fixture.automation.HasRequest(129));
        for (std::uint64_t id = 1; id <= 3; ++id)
            REQUIRE(fixture.automation.Cancel(id) == AudioAutomationStatus::Ok);
        CHECK(fixture.transitions.Apply(prepared) == MixerSnapshotStatus::Ok);
        CHECK(fixture.automation.HasRequest(128));
        CHECK(fixture.automation.HasRequest(131));
    }

    TEST_CASE("Mixer snapshot curves retain exact endpoints and sample continuity", "[audio][snapshot][curves]") {
        for (const auto curve : {AudioAutomationCurve::Immediate, AudioAutomationCurve::Linear, AudioAutomationCurve::Smoothstep}) {
            Fixture fixture;
            REQUIRE(fixture.transitions.Apply(fixture.Prepare(1, 1, 0, 0, curve)) == MixerSnapshotStatus::Ok);
            float previous{};
            for (std::uint64_t frame = 0; frame <= 160; ++frame) {
                REQUIRE(fixture.automation.Advance(Clock(frame)) == AudioAutomationStatus::Ok);
                const auto value = fixture.Value();
                CHECK(std::abs(value - previous) <= 0.011001F);
                previous = value;
            }
            CHECK(previous == 1);
        }
    }

    TEST_CASE("Mixer snapshots consume one retained SPSC batch and render smoothstep bus send effect values allocation free",
              "[audio][snapshot][realtime]") {
        Fixture fixture;
        auto prepared = fixture.Prepare(1, 1, 20, 0, AudioAutomationCurve::Smoothstep);
        VerifyTransport(prepared);
        REQUIRE(fixture.automation.Advance(Clock(20)) == AudioAutomationStatus::Ok);
        CoreAudioDSPNode gain{CoreAudioDSPKind::Gain, {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)}, 1};
        alignas(64) std::array<std::byte, 1024> state{};
        REQUIRE(gain.Prepare({1, state, {}}).HasValue());
        alignas(64) float inputSample{};
        alignas(64) float outputSample{};
        std::array<float *, 1> inputPlanes{&inputSample};
        std::array<float *, 1> outputPlanes{&outputSample};
        const auto layout = ViewAudioChannelLayout(gain.Descriptor().inputs[0].format.layout);
        std::array<AudioDSPPortBuffer, 1> inputs{{{true, {layout, 48'000, inputPlanes, 1, 1}}}};
        std::array<AudioDSPPortBuffer, 1> outputs{{{true, {layout, 48'000, outputPlanes, 0, 1}}}};
        std::array<AudioDSPParameterValue, 1> parameters{{{gain.Descriptor().parameters[0].identity, 0, 0, 0}}};
        AudioDSPProcessContext process{.inputs = inputs, .outputs = outputs, .parameters = parameters, .stateStorage = state, .frames = 1};
        const auto allocations = Tests::AllocationProbe::Count();
        const auto frees = Tests::AllocationProbe::FreeCount();
        const auto applied = fixture.transitions.Apply(prepared);
        std::array<float, 161> output{};
        bool successful = applied == MixerSnapshotStatus::Ok;
        for (std::uint64_t frame = 20; frame <= 180; ++frame) {
            successful = successful && fixture.automation.Advance(Clock(frame)) == AudioAutomationStatus::Ok;
            float bus{}, send{}, effect{};
            successful = successful && fixture.automation.Value(fixture.bindings[0].address, bus) == AudioAutomationStatus::Ok;
            successful = successful && fixture.automation.Value(fixture.bindings[1].address, send) == AudioAutomationStatus::Ok;
            successful = successful && fixture.automation.Value(fixture.bindings[2].address, effect) == AudioAutomationStatus::Ok;
            inputSample = bus * send;
            parameters[0].value = parameters[0].target = effect;
            successful = successful && gain.Process(process).status == AudioDSPProcessStatus::Processed;
            output[frame - 20] = outputSample;
        }
        const auto finalAllocations = Tests::AllocationProbe::Count();
        const auto finalFrees = Tests::AllocationProbe::FreeCount();
        CHECK(successful);
        CHECK(finalAllocations == allocations);
        CHECK(finalFrees == frees);
        CHECK(output.front() == 0);
        CHECK(output.back() == 1);
        for (std::size_t frame = 0; frame < output.size(); ++frame) {
            const auto fraction = static_cast<float>(frame) / 160;
            const auto value = fraction * fraction * (3 - 2 * fraction);
            CHECK(output[frame] == Catch::Approx(value * value * value).margin(2e-6));
        }
        CHECK(fixture.transitions.Apply(fixture.Prepare(2, 4, 180, -10)) == MixerSnapshotStatus::Ok);
    }
}  // namespace Horo::Audio
