#include "Horo/Vfx/VfxWorld.h"
#include "VfxWorldCoordinates.h"
#include "support/AllocationProbe.h"
#include "support/VfxGpuTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Vfx;

    VfxEmitterPreparation CpuBinding() {
        const auto scope = VfxIdentityScope::Create(42).Value();
        return {.effect = {scope, 0, 1},
                .emitter = {scope, 0, 1},
                .particles = {.buffer = {scope, 0, 1}, .capacity = 2},
                .material = {1},
                .materialBlend = Render::MaterialAlphaMode::Additive};
    }

    VfxExtractionBudget Budget() {
        return {4, 4, 4, 2, 1024U * 1024U};
    }

    std::unique_ptr<VfxWorld> CpuWorld() {
        const std::array cpu{CpuBinding()};
        auto result = VfxWorld::Prepare(cpu[0].effect.scope, cpu, {}, Budget());
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

TEST_CASE("Mixed world rejects unbound GPU preparation without consuming caller owners", "[vfx][gpu][world]") {
    Vfx::Tests::GpuSeedFixture seed;
    auto simulator = seed.Prepare();
    const auto state = simulator->RenderState();
    CHECK_FALSE(state.active);
    CHECK(state.generation == 1);
    CHECK(state.nextTick == 0);
    CHECK_FALSE(state.preparation.IsLive());
    const auto info = seed.Info();
    const std::array cpu{CpuBinding()};
    std::array<VfxGpuEmitterPreparation, 1> gpu;
    gpu[0] = {info.activation, seed.Source().emitter, std::move(simulator)};
    auto *original = gpu[0].simulator.get();
    const auto result = VfxWorld::Prepare(info.scene, cpu, gpu, Budget());
    REQUIRE(result.HasError());
    REQUIRE(gpu[0].simulator.get() == original);
    CHECK_FALSE(original->LifetimeCancellation().IsCancellationRequested());
    CHECK(original->PrepareStep(0, .25F).status == GpuParticleStepStatus::NotActive);
}

TEST_CASE("Mixed prepare preserves the existing CPU snapshot and restart behavior", "[vfx][gpu][world]") {
    auto world = CpuWorld();
    const auto binding = CpuBinding();
    REQUIRE(world->Play(binding.effect).HasValue());
    auto particles = world->Particles(binding.effect, binding.emitter);
    REQUIRE(particles.HasValue());
    REQUIRE(particles.Value()->Spawn(ParticleSimulationId::Create(9).Value()).HasValue());
    auto view = particles.Value()->View();
    REQUIRE(view.HasValue());
    view.Value().sizeX[0] = 1;
    view.Value().sizeY[0] = 1;
    auto frame = world->Extract(1, {}, 1);
    REQUIRE(frame.report.status == VfxExtractionStatus::Complete);
    CHECK(frame.snapshot.Instances().size() == 1);
    CHECK(frame.snapshot.GpuBatches().empty());
    auto stopped = world->Stop(binding.effect);
    REQUIRE(stopped.HasValue());
    CHECK(world->Play(stopped.Value()).HasValue());
    REQUIRE(world->Shutdown().HasValue());
    world.reset();
    CHECK(frame.snapshot.Instances()[0].particle == 9);
    CHECK(frame.snapshot.GpuBatches().empty());
}

TEST_CASE("World GPU safe phases are bounded allocation free and enforce owner lifetime", "[vfx][gpu][world]") {
    auto world = CpuWorld();
    VfxGpuUpdateReport steps, acks;
    std::size_t allocations{};
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        steps = world->PrepareGpuSteps(.25F);
        acks = world->ApplyGpuAcknowledgements({});
        allocations = measurement.Snapshot().requests;
    }
    CHECK(allocations == 0);
    CHECK(steps.status == GpuParticleStepStatus::Complete);
    CHECK(acks.processed == 0);
    std::array<Render::GpuVfxStepAcknowledgement, VfxWorld::MaximumGpuAcknowledgements + 1> excessive;
    CHECK(world->ApplyGpuAcknowledgements(excessive).status == GpuParticleStepStatus::CapacityExceeded);
    GpuParticleStepStatus foreign{};
    std::thread worker([&] {
        foreign = world->PrepareGpuSteps(.25F).status;
    });
    worker.join();
    CHECK(foreign == GpuParticleStepStatus::ThreadViolation);
    REQUIRE(world->Shutdown().HasValue());
    CHECK(world->PrepareGpuSteps(.25F).status == GpuParticleStepStatus::ShutDown);
}

TEST_CASE("World refuses stale GPU acknowledgements and replacement without fabricated authority", "[vfx][gpu][world]") {
    auto world = CpuWorld();
    const std::array<Render::GpuVfxStepAcknowledgement, 1> stale{};
    const auto report = world->ApplyGpuAcknowledgements(stale);
    CHECK(report.status == GpuParticleStepStatus::StaleAcknowledgement);
    CHECK(report.rejected == 1);
    Vfx::Tests::GpuSeedFixture seed;
    std::array<VfxGpuEmitterPreparation, 1> replacement;
    replacement[0] = {seed.Info().activation, seed.Source().emitter, seed.Prepare()};
    auto *owner = replacement[0].simulator.get();
    CHECK(world->ReprepareGpuEffect(seed.Info().activation, replacement).HasError());
    CHECK(replacement[0].simulator.get() == owner);
}

TEST_CASE("World origin conversion preserves nearby extremes and safely handles opposite signed cells", "[vfx][world][precision]") {
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    CHECK(Horo::Vfx::Detail::VfxRelativeAxis({high, 1}, {high - 1, 0}) == 1024.001F);
    CHECK(Horo::Vfx::Detail::VfxRelativeAxis({low + 1, 1}, {low, 0}) == 1024.001F);
    CHECK(Horo::Vfx::Detail::VfxRelativeAxis({high - 1, 0}, {high, 1}) == -1024.001F);
    const auto positive = Horo::Vfx::Detail::VfxRelativeAxis({high, 0}, {low, 0});
    const auto negative = Horo::Vfx::Detail::VfxRelativeAxis({low, 0}, {high, 0});
    CHECK(std::isfinite(positive));
    CHECK(positive > 0);
    CHECK(negative == -positive);
    CHECK(Horo::Vfx::Detail::VfxRelativeAxis({high, 7}, {high, 2}, .125F) == .130F);
    CHECK_FALSE(std::isfinite(Horo::Vfx::Detail::VfxRelativeAxis({}, {}, std::numeric_limits<float>::infinity())));
}
