#pragma once
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "Horo/Vfx/GpuParticleSimulator.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <utility>

namespace Horo::Vfx::Tests {
    /** @brief Produces a genuine cook admission using explicitly available fixture material evidence and the canonical compact tier. */
    inline ParticleCookAdmission CookGpuParticleSource(ParticleSystemDescriptorData source) {
        const auto descriptor = ParticleDescriptor(std::move(source));
        const auto profile = GetParticleCookProfile(ParticleCookTier::Compact);
        REQUIRE(profile.HasValue());
        const ParticleMaterialEvidence material{descriptor.Data().material, ParticleMaterialAvailability::Available};
        auto cooked = BuildParticleSystemCookPlan(descriptor, profile.Value(), material, ParticleErrorRegistry(), "fixture-gpu-particles");
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());
        REQUIRE(cooked.Value().admission.has_value());
        return *cooked.Value().admission;
    }

    /** @brief Authored input-only seed fixture; this never fabricates a native resource or submission receipt. */
    struct GpuSeedFixture final {
        std::array<std::array<float, 2>, 12> floats{};
        std::array<std::uint32_t, 2> colors{0xff0000ff, 0x00ff00ff}, flags{};
        std::array<ParticleSimulationId, 2> identities{ParticleSimulationId::Create(1001).Value(),
                                                       ParticleSimulationId::Create(1002).Value()};
        std::array<CpuParticleForceModule, 1> forces{{{.kind = CpuParticleForceKind::Gravity, .vector = {0, -1, 0}}}};

        GpuSeedFixture() {
            floats[6] = {1, 1};
            floats[7] = {1, 1};
            floats[11] = {2, 3};
        }

        ParticleSystemDescriptorData Source() const {
            auto d = ValidParticleDescriptorData();
            d.simulationPreference = SimulationPreference::RequireGPU;
            d.maximumParticles = 2;
            d.spawnRate = {0, 0};
            d.initialSpeed = {0, 0};
            d.initialSize = {1, 1};
            d.initialOpacity = {1, 1};
            d.lifetimeSeconds = {2, 3};
            d.collisionMode = ParticleCollisionMode::None;
            d.sortMode = ParticleSortMode::None;
            return d;
        }

        GpuParticleSimulatorCreateInfo Info() const {
            const auto scene = VfxIdentityScope::Create(42).Value();
            GpuParticleSimulatorCreateInfo i;
            i.scene = scene;
            i.activation = MakeVfxIdentity<EffectSystemIdentityTag>(scene, 2, 1).Value();
            i.buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scene, 5, 1).Value();
            i.effectSeed = 23;
            i.resolution = {VfxQualityProfile::Standard,
                            VfxQualityProfile::Standard,
                            ResolvedSimulationDomain::GPU,
                            VfxDegradation::None,
                            0,
                            2,
                            2U * 1024U * 1024U,
                            VfxCapabilityRevision::Create(1).Value(),
                            VfxQualityPolicyRevision::Create(1).Value()};
            auto &a = i.initial.attributes;
            a.positionX = floats[0];
            a.positionY = floats[1];
            a.positionZ = floats[2];
            a.velocityX = floats[3];
            a.velocityY = floats[4];
            a.velocityZ = floats[5];
            a.sizeX = floats[6];
            a.sizeY = floats[7];
            a.rotation = floats[8];
            a.angularVelocity = floats[9];
            a.age = floats[10];
            a.maximumAge = floats[11];
            a.packedColor = colors;
            a.customFlags = flags;
            i.initial.identities = identities;
            i.forces = forces;
            i.layoutLimits = {1024U * 1024U, 1024U * 1024U};
            i.maximumRetainedBytes = 1024U * 1024U;
            i.maximumPendingSteps = 2;
            i.maximumSteps = 100;
            i.maximumDeltaSeconds = .25F;
            return i;
        }

        std::unique_ptr<GpuParticleSimulator> Prepare() const {
            auto result = GpuParticleSimulator::Create(CookGpuParticleSource(Source()), Info());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };
}  // namespace Horo::Vfx::Tests
