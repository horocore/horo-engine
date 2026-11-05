#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] VfxIdentityScope Scope() {
            return VfxIdentityScope::Create(77).Value();
        }

        [[nodiscard]] ParticleBufferId BufferId(const std::uint32_t slot) {
            return MakeVfxIdentity<ParticleBufferIdentityTag>(Scope(), slot, 1).Value();
        }

        [[nodiscard]] EffectSystemId Activation() {
            return MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 12, 3).Value();
        }

        [[nodiscard]] ParticleSystemDescriptor Descriptor(const std::uint32_t capacity = 16,
                                                          const ParticleCollisionMode collision = ParticleCollisionMode::None,
                                                          const double lifetime = 10.0) {
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.maximumParticles = capacity;
            data.spawnRate = {0.0, 0.0};
            data.initialSpeed = {0.0, 0.0};
            data.initialSize = {1.0, 1.0};
            data.initialOpacity = {1.0, 1.0};
            data.lifetimeSeconds = {lifetime, lifetime};
            data.collisionMode = collision;
            return Tests::ParticleDescriptor(std::move(data));
        }

        template <typename Value> [[nodiscard]] bool HasError(const Result<Value> &result, const ErrorCodeDescriptor &descriptor) {
            return result.HasError() && result.ErrorValue().code.Value() == descriptor.code.Value();
        }

        struct StageTrace final {
            std::array<CpuParticleStage, 7> stages{};
            std::uint32_t count{};
            std::int32_t failAfter{-1};
        };

        Result<void> ObserveStage(void *context, const CpuParticleStage stage) noexcept {
            auto &trace = *static_cast<StageTrace *>(context);
            if (trace.count < trace.stages.size())
                trace.stages[trace.count] = stage;
            const std::uint32_t current = trace.count++;
            if (trace.failAfter >= 0 && current == static_cast<std::uint32_t>(trace.failAfter))
                return Result<void>::Failure(MakeError(VfxErrors::ParticleStageContractViolation));
            return Result<void>::Success();
        }

        Result<CpuParticleCollisionHit> AlwaysHit(void *, const CpuParticleCollisionQueryRequest &request) noexcept {
            return Result<CpuParticleCollisionHit>::Success({.hit = true,
                                                             .position = request.position,
                                                             .normal = {0.0F, 1.0F, 0.0F},
                                                             .distance = 0.0F,
                                                             .restitution = 1.0F,
                                                             .stableTarget = 9,
                                                             .stableFeature = 2});
        }

        [[nodiscard]] CpuParticleSimulator Simulator(const ParticleSystemDescriptor &descriptor, const std::uint32_t slot,
                                                     const std::uint64_t seed = 0xD00D) {
            auto result =
                CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(slot),
                                                          .activation = Activation(),
                                                          .effectSeed = seed,
                                                          .maximumBurstParticles = CpuParticleSimulationHardLimits::BurstParticles});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct LegacyReentry final {
            CpuParticleSimulator *simulation{};
            bool observed{};
            CpuParticleAdvanceOutcome reentry;
        };

        void ObserveLegacyAllocation(std::size_t, void *context) noexcept {
            auto &probe = *static_cast<LegacyReentry *>(context);
            if (probe.observed)
                return;
            probe.observed = true;
            probe.reentry = probe.simulation->AdvancePrepared({.tick = 0});
        }

        struct FailureProvider final {
            std::uint32_t calls{};
        };

        Result<CpuParticleCollisionHit> ProviderFailure(void *context, const CpuParticleCollisionQueryRequest &) noexcept {
            ++static_cast<FailureProvider *>(context)->calls;
            return Result<CpuParticleCollisionHit>::Failure(
                MakeError(VfxErrors::ParticleCollisionQueryUnavailable, "fixture provider cause"));
        }
    }  // namespace

    TEST_CASE("CPU simulator executes the fixed seven-stage order and commits atomically", "[unit][vfx][particle-simulator]") {
        StageTrace trace{};
        auto descriptor = Descriptor();
        auto result = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(1),
                                                                .activation = Activation(),
                                                                .effectSeed = 10,
                                                                .maximumBurstParticles = 4,
                                                                .stageObserver = ObserveStage,
                                                                .stageObserverContext = &trace});
        REQUIRE(result.HasValue());
        CpuParticleSimulator simulator = std::move(result).Value();

        const auto step = simulator.Advance({.deltaSeconds = 0.25F, .burstCount = 2, .tick = 0});
        REQUIRE(step.HasValue());
        REQUIRE(trace.count == CpuParticleStageOrder.size());
        for (std::size_t index = 0; index < CpuParticleStageOrder.size(); ++index)
            CHECK(trace.stages[index] == CpuParticleStageOrder[index]);
        CHECK(step.Value().spawned == 2);
        CHECK(step.Value().active == 2);
        CHECK(simulator.Extract().Value().committedGeneration == 1);
    }

    TEST_CASE("CPU simulator is deterministic and steady-state stepping allocates nothing", "[unit][vfx][particle-simulator]") {
        const std::array
            forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0.0F, -2.0F, 0.0F}, .strength = 1.0F},
                   CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.25F, .frequency = 0.5F, .randomChannel = 19}};
        const auto descriptor = Descriptor(32, ParticleCollisionMode::None, 0.01);
        auto firstResult = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(2),
                                                                     .activation = Activation(),
                                                                     .effectSeed = 0xA55A,
                                                                     .maximumBurstParticles = 32,
                                                                     .forces = forces});
        auto secondResult = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(3),
                                                                      .activation = Activation(),
                                                                      .effectSeed = 0xA55A,
                                                                      .maximumBurstParticles = 32,
                                                                      .forces = forces});
        REQUIRE(firstResult.HasValue());
        REQUIRE(secondResult.HasValue());
        CpuParticleSimulator first = std::move(firstResult).Value();
        CpuParticleSimulator second = std::move(secondResult).Value();
        REQUIRE(first.Advance({.deltaSeconds = 0.01F, .burstCount = 32, .tick = 0}).HasValue());
        REQUIRE(second.Advance({.deltaSeconds = 0.01F, .burstCount = 32, .tick = 0}).HasValue());

        const auto left = first.Extract().Value();
        const auto right = second.Extract().Value();
        REQUIRE(left.positionX.size() == right.positionX.size());
        for (std::size_t index = 0; index < left.positionX.size(); ++index) {
            CHECK(left.positionX[index] == right.positionX[index]);
            CHECK(left.positionY[index] == right.positionY[index]);
            CHECK(left.positionZ[index] == right.positionZ[index]);
            CHECK(left.velocityX[index] == right.velocityX[index]);
            CHECK(left.velocityY[index] == right.velocityY[index]);
            CHECK(left.velocityZ[index] == right.velocityZ[index]);
            CHECK(left.packedColor[index] == right.packedColor[index]);
        }

        const std::size_t before = ::Horo::Tests::AllocationProbe::Count();
        bool succeeded = true;
        for (std::uint64_t tick = 1; tick < 1001; ++tick) {
            const auto stepped = first.Advance({.deltaSeconds = 0.01F, .burstCount = 32, .tick = tick});
            succeeded = succeeded && stepped.HasValue() && stepped.Value().spawned == 32 && stepped.Value().killed == 32;
        }
        const std::size_t after = ::Horo::Tests::AllocationProbe::Count();
        CHECK(succeeded);
        CHECK(after == before);
    }

    TEST_CASE("CPU simulator preserves the committed generation on cancellation and stage failure", "[unit][vfx][particle-simulator]") {
        auto cancelled = Simulator(Descriptor(4), 4);
        REQUIRE(cancelled.Advance({.burstCount = 1, .tick = 0}).HasValue());
        const auto before = cancelled.Statistics();
        CHECK(HasError(cancelled.Advance({.burstCount = 1, .tick = 1, .cancelled = true}), VfxErrors::ParticleSimulationStepCancelled));
        CHECK(cancelled.Statistics().committedGeneration == before.committedGeneration);
        CHECK(cancelled.Statistics().active == before.active);
        REQUIRE(cancelled.Advance({.burstCount = 1, .tick = 1}).HasValue());

        StageTrace trace{.failAfter = 2};
        auto result = CpuParticleSimulator::Create(Descriptor(4), {.buffer = BufferId(5),
                                                                   .activation = Activation(),
                                                                   .maximumBurstParticles = 4,
                                                                   .stageObserver = ObserveStage,
                                                                   .stageObserverContext = &trace});
        REQUIRE(result.HasValue());
        CpuParticleSimulator failed = std::move(result).Value();
        CHECK(HasError(failed.Advance({.burstCount = 1, .tick = 0}), VfxErrors::ParticleStageContractViolation));
        CHECK(failed.Statistics().committedGeneration == 0);
        CHECK(failed.Statistics().active == 0);
    }

    TEST_CASE("CPU simulator exposes typed collision and gameplay payload seams", "[unit][vfx][particle-simulator]") {
        const std::array channels{CpuParticlePayloadChannel{.channel = 1,
                                                            .classification = CpuParticlePayloadClass::GameplayInput,
                                                            .customFloatStream = 0,
                                                            .minimum = 0.0F,
                                                            .maximum = 1.0F},
                                  CpuParticlePayloadChannel{.channel = 2,
                                                            .classification = CpuParticlePayloadClass::GameplayOutput,
                                                            .customFloatStream = 1,
                                                            .minimum = 0.0F,
                                                            .maximum = 1.0F}};
        const auto descriptor = Descriptor(4, ParticleCollisionMode::PhysicsWorld);
        auto result = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(6),
                                                                .activation = Activation(),
                                                                .maximumBurstParticles = 4,
                                                                .customFloatStreams = 2,
                                                                .physicsWorld = {.probe = AlwaysHit, .required = true},
                                                                .payloadChannels = channels});
        REQUIRE(result.HasValue());
        CpuParticleSimulator simulator = std::move(result).Value();
        REQUIRE(simulator.SubmitGameplayInput(1, 0.75F).HasValue());
        CHECK(HasError(simulator.SubmitGameplayInput(2, 0.5F), VfxErrors::ParticleGameplayAccessDenied));
        const auto step = simulator.Advance({.deltaSeconds = 0.1F, .burstCount = 1, .tick = 0});
        REQUIRE(step.HasValue());
        CHECK(step.Value().collisions == 1);
        CHECK(step.Value().killed == 0);
        const auto generation = simulator.Extract().Value().committedGeneration;
        CHECK(simulator.ReadGameplayOutput(2, 0, generation).HasValue());
        CHECK(HasError(simulator.ReadGameplayOutput(1, 0, generation), VfxErrors::ParticleGameplayAccessDenied));
        CHECK(HasError(simulator.ReadGameplayOutput(2, 0, generation - 1), VfxErrors::ParticleGenerationStale));
    }

    TEST_CASE("CPU simulator rejects mandatory capacity overflow without mutating state", "[unit][vfx][particle-simulator]") {
        const auto descriptor = Descriptor(1);
        auto result = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(7),
                                                                .activation = Activation(),
                                                                .maximumBurstParticles = 4,
                                                                .requiredGameplay = true});
        REQUIRE(result.HasValue());
        CpuParticleSimulator simulator = std::move(result).Value();
        CHECK(HasError(simulator.Advance({.burstCount = 2, .tick = 0}), VfxErrors::ParticleStepCapacityExceeded));
        CHECK(simulator.Statistics().committedGeneration == 0);
        CHECK(simulator.Statistics().active == 0);
        REQUIRE(simulator.Advance({.burstCount = 1, .tick = 0}).HasValue());
    }

    TEST_CASE("CPU prepared reset preserves monotonic particle tick and generation identities", "[unit][vfx][particle-simulator][reset]") {
        auto simulator = Simulator(Descriptor(4), 7);
        REQUIRE(simulator.Advance({.deltaSeconds = 0.125F, .burstCount = 2, .tick = 0}).HasValue());
        const auto old = simulator.HandleAtDenseIndex(0);
        REQUIRE(old.HasValue());
        const auto before = simulator.Statistics();
        auto next = Activation();
        ++next.generation;
        const auto bytes = simulator.PreparedBytes();
        Horo::Tests::AllocationProbe::Measurement measured;
        CpuParticleResetStatus status;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            status = simulator.Reset(next);
            measured = probe.Snapshot();
        }
        REQUIRE(status == CpuParticleResetStatus::Complete);
        CHECK(measured.requests == 0);
        CHECK(simulator.PreparedBytes() == bytes);
        CHECK(simulator.Statistics().active == 0);
        CHECK(simulator.Statistics().steps == before.steps);
        CHECK(simulator.Statistics().committedGeneration > before.committedGeneration);
        REQUIRE(simulator.Advance({.deltaSeconds = 0.125F, .burstCount = 1, .tick = 1}).HasValue());
        const auto current = simulator.HandleAtDenseIndex(0);
        REQUIRE(current.HasValue());
        CHECK(current.Value().particle.Value() > old.Value().particle.Value());
        CHECK(simulator.SignalKill(old.Value()).HasError());
    }

    TEST_CASE("CPU prepared reset rejects stale foreign and shutdown activation before mutation",
              "[unit][vfx][particle-simulator][reset]") {
        auto simulator = Simulator(Descriptor(4), 7);
        REQUIRE(simulator.Advance({.burstCount = 1, .tick = 0}).HasValue());
        const auto before = simulator.Statistics();
        CHECK(simulator.Reset(Activation()) == CpuParticleResetStatus::InvalidActivation);
        auto foreign = Activation();
        ++foreign.slot;
        ++foreign.generation;
        CHECK(simulator.Reset(foreign) == CpuParticleResetStatus::InvalidActivation);
        CHECK(simulator.Statistics().active == before.active);
        CHECK(simulator.Statistics().committedGeneration == before.committedGeneration);
        REQUIRE(simulator.Shutdown().HasValue());
        CHECK(simulator.Reset(foreign) == CpuParticleResetStatus::ShutDown);
    }

    TEST_CASE("Prepared CPU advance shares exact canonical trajectories and generations with legacy Advance",
              "[unit][vfx][particle-simulator][prepared]") {
        auto legacy = Simulator(Descriptor(8), 9);
        auto prepared = Simulator(Descriptor(8), 9);
        for (std::uint64_t tick = 0; tick < 10; ++tick) {
            const CpuParticleSimulationStep step{0.125F, 1, tick, false};
            const auto left = legacy.Advance(step);
            const auto right = prepared.AdvancePrepared(step);
            REQUIRE(left.HasValue());
            REQUIRE(right.status == CpuParticleAdvanceStatus::Complete);
            CHECK(left.Value() == right.step);
            const auto leftView = legacy.Extract();
            const auto rightView = prepared.Extract();
            REQUIRE(leftView.HasValue());
            REQUIRE(rightView.HasValue());
            CHECK(std::ranges::equal(leftView.Value().positionX, rightView.Value().positionX));
            CHECK(std::ranges::equal(leftView.Value().positionY, rightView.Value().positionY));
            CHECK(std::ranges::equal(leftView.Value().age, rightView.Value().age));
        }
    }

    TEST_CASE("Prepared mandatory capacity failure preserves exact code and committed state without allocation",
              "[unit][vfx][particle-simulator][prepared][allocation]") {
        auto created = CpuParticleSimulator::Create(Descriptor(1), {.buffer = BufferId(9),
                                                                    .activation = Activation(),
                                                                    .maximumBurstParticles = 4,
                                                                    .requiredGameplay = true});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        Horo::Tests::AllocationProbe::Measurement measured;
        CpuParticleAdvanceOutcome result;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            result = simulation.AdvancePrepared({.burstCount = 2, .tick = 0});
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        REQUIRE(result.status == CpuParticleAdvanceStatus::Rejected);
        REQUIRE(result.failure == &VfxErrors::ParticleStepCapacityExceeded);
        CHECK(simulation.Statistics().committedGeneration == 0);
        CHECK(simulation.Statistics().active == 0);
        const auto legacy = simulation.Advance({.burstCount = 2, .tick = 0});
        REQUIRE(legacy.HasError());
        CHECK(legacy.ErrorValue().code.Value() == result.failure->code.Value());
        CHECK(legacy.ErrorValue().message == result.failure->summary);
        CHECK(simulation.AdvancePrepared({.burstCount = 1, .tick = 0}).status == CpuParticleAdvanceStatus::Complete);
    }

    TEST_CASE("Prepared CPU front guards reject cancellation invalid ticks and oversized steps without allocation",
              "[unit][vfx][particle-simulator][prepared][allocation]") {
        auto simulation = Simulator(Descriptor(4), 9);
        const std::array steps{CpuParticleSimulationStep{.tick = 0, .cancelled = true}, CpuParticleSimulationStep{.tick = 1},
                               CpuParticleSimulationStep{.deltaSeconds = -1.0F, .tick = 0},
                               CpuParticleSimulationStep{.deltaSeconds = std::numeric_limits<float>::infinity(), .tick = 0},
                               CpuParticleSimulationStep{.burstCount = CpuParticleSimulationHardLimits::BurstParticles + 1, .tick = 0}};
        std::array<CpuParticleAdvanceOutcome, 5> results;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            for (std::size_t index = 0; index < steps.size(); ++index)
                results[index] = simulation.AdvancePrepared(steps[index]);
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(results[0].failure == &VfxErrors::ParticleSimulationStepCancelled);
        for (std::size_t index = 1; index < results.size(); ++index)
            CHECK(results[index].failure == &VfxErrors::ParticleSpawnStepInvalid);
        CHECK(simulation.Statistics().steps == 0);
        CHECK(simulation.Statistics().committedGeneration == 0);
    }

    TEST_CASE("Prepared CPU admission refuses legacy observers while legacy Advance preserves all seven callbacks",
              "[unit][vfx][particle-simulator][prepared]") {
        StageTrace trace;
        auto created = CpuParticleSimulator::Create(Descriptor(4), {.buffer = BufferId(9),
                                                                    .activation = Activation(),
                                                                    .maximumBurstParticles = 4,
                                                                    .stageObserver = ObserveStage,
                                                                    .stageObserverContext = &trace});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        CHECK(simulation.AdvancePrepared({.burstCount = 1, .tick = 0}).status == CpuParticleAdvanceStatus::UnsupportedDiagnostics);
        CHECK(trace.count == 0);
        REQUIRE(simulation.Advance({.burstCount = 1, .tick = 0}).HasValue());
        CHECK(trace.count == 7);
    }

    TEST_CASE("Legacy provider failure retains exact immutable cause and prepared execution never invokes it",
              "[unit][vfx][particle-simulator][prepared]") {
        FailureProvider provider;
        auto created =
            CpuParticleSimulator::Create(Descriptor(4, ParticleCollisionMode::PhysicsWorld), {.buffer = BufferId(9),
                                                                                              .activation = Activation(),
                                                                                              .maximumBurstParticles = 4,
                                                                                              .physicsWorld = {.context = &provider,
                                                                                                               .probe = ProviderFailure,
                                                                                                               .sceneGeneration = 1,
                                                                                                               .snapshotGeneration = 1,
                                                                                                               .required = true}});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        CHECK(simulation.AdvancePrepared({.burstCount = 1, .tick = 0}).status == CpuParticleAdvanceStatus::UnsupportedDiagnostics);
        CHECK(provider.calls == 0);
        const auto legacy = simulation.Advance({.burstCount = 1, .tick = 0});
        REQUIRE(legacy.HasError());
        REQUIRE(legacy.ErrorValue().cause.Get());
        CHECK(legacy.ErrorValue().code.Value() == VfxErrors::ParticleCollisionQueryFailed.code.Value());
        CHECK(legacy.ErrorValue().cause.Get()->code.Value() == VfxErrors::ParticleCollisionQueryUnavailable.code.Value());
        CHECK(legacy.ErrorValue().cause.Get()->message == "fixture provider cause");
        CHECK(provider.calls == 1);
        CHECK(simulation.Statistics().committedGeneration == 0);
    }

    TEST_CASE("Legacy stage error materialization retains reentry fence until the public Result is complete",
              "[unit][vfx][particle-simulator][prepared]") {
        auto created = CpuParticleSimulator::Create(Descriptor(1), {.buffer = BufferId(9),
                                                                    .activation = Activation(),
                                                                    .maximumBurstParticles = 4,
                                                                    .requiredGameplay = true});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        LegacyReentry probe{&simulation};
        std::optional<Result<CpuParticleSimulationStepResult>> result;
        {
            Horo::Tests::AllocationProbe::ScopedObserver observer{ObserveLegacyAllocation, &probe};
            result.emplace(simulation.Advance({.burstCount = 2, .tick = 0}));
        }
        REQUIRE(result);
        CHECK(result->HasError());
        REQUIRE(probe.observed);
        CHECK(probe.reentry.status == CpuParticleAdvanceStatus::Rejected);
        CHECK(probe.reentry.failure == &VfxErrors::ParticleStageContractViolation);
        CHECK(simulation.Statistics().committedGeneration == 0);
    }

    TEST_CASE("Prepared continuous mandatory capacity exhaustion preserves the last real commit without allocation",
              "[unit][vfx][particle-simulator][prepared][allocation]") {
        auto data = Descriptor(2).Data();
        data.spawnRate = {2.0, 2.0};
        const auto descriptor = Tests::ParticleDescriptor(data);
        auto created = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(9),
                                                                 .activation = Activation(),
                                                                 .maximumBurstParticles = 2,
                                                                 .requiredGameplay = true});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        REQUIRE(simulation.AdvancePrepared({.deltaSeconds = 0.5F, .tick = 0}).status == CpuParticleAdvanceStatus::Complete);
        REQUIRE(simulation.AdvancePrepared({.deltaSeconds = 0.5F, .tick = 1}).status == CpuParticleAdvanceStatus::Complete);
        const auto before = simulation.Statistics();
        CpuParticleAdvanceOutcome result;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            result = simulation.AdvancePrepared({.deltaSeconds = 0.5F, .tick = 2});
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(result.failure == &VfxErrors::ParticleStepCapacityExceeded);
        CHECK(simulation.Statistics().committedGeneration == before.committedGeneration);
        CHECK(simulation.Statistics().active == before.active);
        CHECK(simulation.Statistics().steps == before.steps);
    }

    TEST_CASE("Prepared analytic collision arithmetic overflow returns the original static error without allocation",
              "[unit][vfx][particle-simulator][prepared][allocation]") {
        auto data = Descriptor(2, ParticleCollisionMode::Planes).Data();
        data.shape = ParticleEmitterShape::Point;
        const auto descriptor = Tests::ParticleDescriptor(data);
        const auto maximum = std::numeric_limits<float>::max();
        const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {maximum, 0.0F, 0.0F}}};
        const std::array planes{CpuParticlePlane{.point = {-maximum, 0.0F, 0.0F}, .normal = {1.0F, 0.0F, 0.0F}}};
        auto created = CpuParticleSimulator::Create(descriptor, {.buffer = BufferId(9),
                                                                 .activation = Activation(),
                                                                 .maximumBurstParticles = 2,
                                                                 .forces = forces,
                                                                 .planes = planes});
        REQUIRE(created.HasValue());
        auto simulation = std::move(created).Value();
        CpuParticleAdvanceOutcome result;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            result = simulation.AdvancePrepared({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0});
            measured = probe.Snapshot();
        }
        CHECK(measured.requests == 0);
        CHECK(result.status == CpuParticleAdvanceStatus::Rejected);
        CHECK(result.failure == &VfxErrors::ParticleCollisionQueryFailed);
        CHECK(simulation.Statistics().committedGeneration == 0);
        const auto legacy = simulation.Advance({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0});
        REQUIRE(legacy.HasError());
        CHECK(legacy.ErrorValue().code.Value() == result.failure->code.Value());
    }

    TEST_CASE("Prepared CPU wrong-thread and shutdown guards expose static failures before stage access",
              "[unit][vfx][particle-simulator][prepared]") {
        auto simulation = Simulator(Descriptor(2), 9);
        CpuParticleAdvanceOutcome foreign;
        std::thread worker([&] {
            foreign = simulation.AdvancePrepared({.tick = 0});
        });
        worker.join();
        CHECK(foreign.failure == &VfxErrors::ParticleBufferThreadViolation);
        REQUIRE(simulation.Shutdown().HasValue());
        CHECK(simulation.AdvancePrepared({.tick = 0}).failure == &VfxErrors::ParticleBufferShutDown);
    }
}  // namespace Horo::Vfx
