#include "support/TypedIdentityTestSupport.h"
#include "support/VfxGpuTestSupport.h"

#include <bit>
#include <limits>

using namespace Horo;
using namespace Horo::Vfx;

TEST_CASE("GPU logical preparation copies canonical seeds and remains unbound before actual renderer admission", "[vfx][gpu-logical]") {
    Vfx::Tests::GpuSeedFixture input;
    auto owner = input.Prepare();
    const auto &p = owner->Preparation();
    CHECK(Render::ValidateVfxGpuSimulationPreparation(p).HasValue());
    CHECK(p.descriptor.initialParticleCount == 2);
    CHECK_FALSE(p.descriptor.readback.enabled);
    CHECK(p.descriptor.random.effectSeed == 23);
    CHECK(owner->PrepareStep(0, .1F).status == GpuParticleStepStatus::NotActive);
    CHECK(owner->PendingWork().empty());
    input.floats[0][0] = 999;
    input.forces[0].strength = 77;
    CHECK(p.initialWords[0] == std::bit_cast<std::uint32_t>(0.0F));
    CHECK(p.forceWords[Render::GpuParticleForceWords::Strength] == std::bit_cast<std::uint32_t>(1.0F));
    Horo::Tests::RequireActionableError(owner->Activate({}), VfxErrors::GpuSimulationReceiptInvalid);
}

TEST_CASE("GPU prepared unit rejects every unsupported authored feature before native activation", "[vfx][gpu-logical]") {
    Vfx::Tests::GpuSeedFixture input;
    auto source = input.Source();
    auto info = input.Info();
    SECTION("continuous spawning") {
        source.spawnRate = {1, 1};
    }
    SECTION("depth collision") {
        source.collisionMode = ParticleCollisionMode::SceneDepth;
    }
    SECTION("sorted transparency") {
        source.sortMode = ParticleSortMode::ByDistance;
    }
    SECTION("over-life program") {
        info.requiredFeatures = GpuParticleRequiredFeature::OverLife;
    }
    SECTION("payload program") {
        info.requiredFeatures = GpuParticleRequiredFeature::PayloadPrograms;
    }
    SECTION("authoritative GPU output") {
        info.requiredFeatures = GpuParticleRequiredFeature::AuthoritativeOutput;
    }
    SECTION("per-step bursts") {
        info.requiredFeatures = GpuParticleRequiredFeature::PerStepBursts;
    }
    SECTION("unknown feature bit") {
        info.requiredFeatures = static_cast<GpuParticleRequiredFeature>(1u << 30);
    }
    SECTION("CPU requirement cannot be relabeled GPU") {
        source.simulationPreference = SimulationPreference::RequireCPU;
    }
    Horo::Tests::RequireActionableError(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(source), info),
                                        VfxErrors::GpuSimulationUnsupported);
}

TEST_CASE("GPU logical payload copies cannot exceed the shared selected memory envelope", "[vfx][gpu-logical]") {
    Vfx::Tests::GpuSeedFixture input;
    auto info = input.Info();
    info.resolution.selectedMemoryBytes = info.maximumRetainedBytes;
    Horo::Tests::RequireActionableError(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(input.Source()), info),
                                        VfxErrors::GpuSimulationInvalid);
}

TEST_CASE("GPU unit lifetime revocation is available before its first step and survives logical shutdown", "[vfx][gpu-logical]") {
    Vfx::Tests::GpuSeedFixture input;
    auto owner = input.Prepare();
    auto token = owner->LifetimeCancellation();
    REQUIRE_FALSE(token.IsCancellationRequested());
    CHECK(owner->Stop() == GpuParticleStepStatus::Complete);
    CHECK(token.IsCancellationRequested());
    CHECK(owner->PendingWork().empty());
    CHECK(owner->PrepareStep(0, 0).status == GpuParticleStepStatus::NotActive);
    CHECK(owner->Shutdown() == GpuParticleStepStatus::Complete);
    CHECK(owner->Shutdown() == GpuParticleStepStatus::Complete);
    CHECK(owner->PrepareStep(0, 0).status == GpuParticleStepStatus::ShutDown);
}
