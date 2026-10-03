#pragma once

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace Horo::Vfx::Tests {
    enum class CpuQualificationScene : std::uint8_t {
        GravityPoint,
        TurbulentBox,
        BouncingPlane
    };

    struct CpuQualificationWorkload final {
        CpuQualificationScene scene;
        const char *name;
        std::uint32_t particles;
    };

    inline constexpr std::array CpuQualificationWorkloads{
        CpuQualificationWorkload{CpuQualificationScene::GravityPoint, "gravity-point", 64},
        CpuQualificationWorkload{CpuQualificationScene::TurbulentBox, "turbulent-box", 512},
        CpuQualificationWorkload{CpuQualificationScene::BouncingPlane, "bouncing-plane", 2048},
    };
    inline constexpr std::uint64_t CpuQualificationSeed = 0xA55A1234ULL;
    inline constexpr float CpuQualificationDeltaSeconds = 1.0F / 60.0F;

    [[nodiscard]] inline ParticleSystemDescriptor CpuQualificationDescriptor(const CpuQualificationWorkload &workload) {
        const auto scope = VfxIdentityScope::Create(42).Value();
        const auto material = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
        ParticleSystemDescriptorData data{.version = CurrentParticleDescriptorSchemaVersion,
                                          .emitter = MakeVfxIdentity<EmitterIdentityTag>(scope, 7, 3).Value(),
                                          .simulationPreference = SimulationPreference::RequireCPU,
                                          .maximumParticles = workload.particles,
                                          .shape = workload.scene == CpuQualificationScene::TurbulentBox ? ParticleEmitterShape::Box
                                                                                                         : ParticleEmitterShape::Point,
                                          .spawnRate = {0.0, 0.0},
                                          .lifetimeKind = ParticleLifetimeKind::Finite,
                                          .lifetimeSeconds = {30.0, 30.0},
                                          .killCondition = ParticleKillCondition::Lifetime,
                                          .initialSpeed = {1.0, 1.0},
                                          .initialSize = {1.0, 1.0},
                                          .initialOpacity = {1.0, 1.0},
                                          .material = material,
                                          .renderMode = ParticleRenderMode::Billboard,
                                          .sortMode = ParticleSortMode::None,
                                          .collisionMode = workload.scene == CpuQualificationScene::BouncingPlane
                                                               ? ParticleCollisionMode::Planes
                                                               : ParticleCollisionMode::None};
        const std::array descriptors{&VfxErrors::ParticleDescriptorMalformed,
                                     &VfxErrors::ParticleDescriptorDuplicate,
                                     &VfxErrors::ParticleDescriptorVersionUnsupported,
                                     &VfxErrors::ParticleDescriptorLimitExceeded,
                                     &VfxErrors::ParticleRangeInvalid,
                                     &VfxErrors::ParticleLifetimeUnbounded,
                                     &VfxErrors::ParticleModeIncompatible,
                                     &VfxErrors::ParticleMaterialMissing,
                                     &VfxErrors::ParticleMaterialTypeMismatch,
                                     &VfxErrors::ParticleMaterialUnloadable,
                                     &VfxErrors::ParticleCookTierExceeded};
        ModuleDescriptor module{.id = {"horo.vfx"},
                                .version = {1, 0, 0},
                                .errorDomains = {
                                    {.id = ErrorDomainId{"horo.vfx"}, .descriptors = {descriptors.begin(), descriptors.end()}}}};
        auto registry = BuildErrorCodeRegistry(std::span{&module, 1});
        if (registry.HasError())
            throw std::runtime_error("CPU qualification error registry failed");
        auto validated = ValidateParticleSystemDescriptor(std::move(data), std::move(registry).Value());
        if (validated.HasError() || !validated.Value().Accepted())
            throw std::runtime_error("CPU qualification descriptor failed validation");
        return *validated.Value().descriptor;
    }

    [[nodiscard]] inline CpuParticleSimulator CpuQualificationSimulator(const CpuQualificationWorkload &workload,
                                                                        const std::uint32_t bufferSlot) {
        const auto scope = VfxIdentityScope::Create(42).Value();
        const auto buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scope, bufferSlot, 1).Value();
        const auto activation = MakeVfxIdentity<EffectSystemIdentityTag>(scope, 10, 2).Value();
        const std::array
            forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0.0F, -9.81F, 0.0F}, .strength = 1.0F},
                   CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.5F, .frequency = 0.5F, .randomChannel = 19}};
        const std::array plane{CpuParticlePlane{.point = {0.0F, -1.0F, 0.0F}, .normal = {0.0F, 1.0F, 0.0F}, .restitution = 0.75F}};
        const auto descriptor = CpuQualificationDescriptor(workload);
        auto result = CpuParticleSimulator::Create(descriptor, {.buffer = buffer,
                                                                .activation = activation,
                                                                .effectSeed = CpuQualificationSeed,
                                                                .maximumBurstParticles = workload.particles,
                                                                .forces = workload.scene == CpuQualificationScene::TurbulentBox
                                                                              ? std::span<const CpuParticleForceModule>{forces}
                                                                              : std::span<const CpuParticleForceModule>{forces.data(), 1},
                                                                .planes = workload.scene == CpuQualificationScene::BouncingPlane
                                                                              ? std::span<const CpuParticlePlane>{plane}
                                                                              : std::span<const CpuParticlePlane>{},
                                                                .collisionResponse = CpuParticleCollisionResponse::Bounce});
        if (result.HasError())
            throw std::runtime_error("CPU qualification simulator preparation failed");
        return std::move(result).Value();
    }

    struct CpuQualificationParticle final {
        std::uint64_t id{};
        std::array<float, 3> position{};
        std::array<float, 3> velocity{};
        std::uint32_t packedColor{};
    };

    struct CpuQualificationSnapshot final {
        std::uint64_t tick{};
        CpuParticleSimulationStepResult step{};
        std::uint64_t identityHash{};
        std::uint64_t colorHash{};
        std::array<CpuQualificationParticle, 3> particles{};
    };

    [[nodiscard]] inline std::uint64_t CpuQualificationHashWord(std::uint64_t hash, const std::uint64_t word,
                                                                const std::uint32_t bytes) noexcept {
        for (std::uint32_t byte = 0; byte < bytes; ++byte) {
            hash ^= static_cast<std::uint8_t>(word >> (byte * 8U));
            hash *= 1'099'511'628'211ULL;
        }
        return hash;
    }

    [[nodiscard]] inline CpuQualificationSnapshot CpuQualificationCapture(CpuParticleSimulator &simulator, const std::uint64_t tick,
                                                                          const CpuParticleSimulationStepResult &step) {
        auto extracted = simulator.Extract();
        if (extracted.HasError() || extracted.Value().positionX.size() < 3)
            throw std::runtime_error("CPU qualification extraction failed");
        const auto &view = extracted.Value();
        CpuQualificationSnapshot snapshot{.tick = tick, .step = step};
        snapshot.identityHash = 14'695'981'039'346'656'037ULL;
        snapshot.colorHash = 14'695'981'039'346'656'037ULL;
        for (std::uint32_t index = 0; index < step.active; ++index) {
            auto handle = simulator.HandleAtDenseIndex(index);
            if (handle.HasError())
                throw std::runtime_error("CPU qualification identity stream failed");
            snapshot.identityHash = CpuQualificationHashWord(snapshot.identityHash, handle.Value().particle.Value(), 8);
            snapshot.colorHash = CpuQualificationHashWord(snapshot.colorHash, view.packedColor[index], 4);
        }
        const std::array indexes{0U, step.active / 2U, step.active - 1U};
        for (std::size_t sample = 0; sample < indexes.size(); ++sample) {
            const auto index = indexes[sample];
            auto handle = simulator.HandleAtDenseIndex(index);
            if (handle.HasError())
                throw std::runtime_error("CPU qualification identity extraction failed");
            snapshot.particles[sample] = {.id = handle.Value().particle.Value(),
                                          .position = {view.positionX[index], view.positionY[index], view.positionZ[index]},
                                          .velocity = {view.velocityX[index], view.velocityY[index], view.velocityZ[index]},
                                          .packedColor = view.packedColor[index]};
        }
        return snapshot;
    }
}  // namespace Horo::Vfx::Tests
