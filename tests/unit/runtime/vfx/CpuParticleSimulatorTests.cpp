#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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
}  // namespace Horo::Vfx
