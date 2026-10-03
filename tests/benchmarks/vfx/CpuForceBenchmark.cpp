#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using namespace Horo::Vfx;
    constexpr std::uint32_t ParticleCount = 4'096;
    constexpr std::uint64_t WarmupTicks = 20;
    constexpr std::uint64_t MeasuredTicks = 200;
    constexpr double BudgetP99Milliseconds = 16.667;

    [[nodiscard]] ParticleSystemDescriptor Descriptor() {
        ParticleSystemDescriptorData data{};
        const auto scope = VfxIdentityScope::Create(91).Value();
        data.emitter = MakeVfxIdentity<EmitterIdentityTag>(scope, 1, 1).Value();
        data.simulationPreference = SimulationPreference::RequireCPU;
        data.maximumParticles = ParticleCount;
        data.shape = ParticleEmitterShape::Point;
        data.spawnRate = {0.0, 0.0};
        data.initialSpeed = {0.0, 0.0};
        data.initialSize = {1.0, 1.0};
        data.initialOpacity = {1.0, 1.0};
        data.lifetimeSeconds = {100.0, 100.0};
        auto material = Horo::Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
        if (material.HasError())
            throw std::runtime_error("Force benchmark material ID failed");
        data.material = material.Value();
        const std::array errors{&VfxErrors::ParticleDescriptorMalformed,
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
        Horo::ModuleDescriptor module{.id = {"horo.vfx"},
                                      .version = {1, 0, 0},
                                      .errorDomains = {
                                          {.id = Horo::ErrorDomainId{"horo.vfx"}, .descriptors = {errors.begin(), errors.end()}}}};
        auto registry = Horo::BuildErrorCodeRegistry(std::span{&module, 1});
        if (registry.HasError())
            throw std::runtime_error("Force benchmark error registry failed");
        auto created = ValidateParticleSystemDescriptor(std::move(data), std::move(registry).Value());
        if (created.HasError() || !created.Value().Accepted())
            throw std::runtime_error("Force benchmark descriptor admission failed");
        return *created.Value().descriptor;
    }
}  // namespace

int main() {
#ifndef NDEBUG
    std::cerr << "Use a Release build without sanitizers or coverage for force qualification.\n";
    return 2;
#else
    const auto scope = VfxIdentityScope::Create(91).Value();
    const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0.0F, -9.81F, 0.0F}},
                            CpuParticleForceModule{.kind = CpuParticleForceKind::Wind, .vector = {1.0F, 0.0F, 0.0F}, .strength = 2.0F},
                            CpuParticleForceModule{.kind = CpuParticleForceKind::Attraction,
                                                   .center = {2.0F, 3.0F, 1.0F},
                                                   .strength = 4.0F,
                                                   .falloff = 0.25F},
                            CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.5F, .randomChannel = 16}};
    auto created = CpuParticleSimulator::Create(Descriptor(), {.buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scope, 2, 1).Value(),
                                                               .activation = MakeVfxIdentity<EffectSystemIdentityTag>(scope, 3, 1).Value(),
                                                               .effectSeed = 0x123456789ABCDEF0ULL,
                                                               .maximumBurstParticles = ParticleCount,
                                                               .forces = forces});
    if (created.HasError())
        throw std::runtime_error("Force benchmark simulator admission failed");
    auto simulator = std::move(created).Value();
    std::vector<double> elapsed(MeasuredTicks);
    for (std::uint64_t tick = 0; tick < WarmupTicks + MeasuredTicks; ++tick) {
        const CpuParticleSimulationStep step{.deltaSeconds = 1.0F / 60.0F, .burstCount = tick == 0 ? ParticleCount : 0U, .tick = tick};
        const auto begin = std::chrono::steady_clock::now();
        const auto result = simulator.Advance(step);
        const auto end = std::chrono::steady_clock::now();
        if (result.HasError() || result.Value().active != ParticleCount)
            throw std::runtime_error("Force benchmark step failed");
        if (tick >= WarmupTicks)
            elapsed[tick - WarmupTicks] = std::chrono::duration<double, std::milli>(end - begin).count();
    }
    const double mean = std::accumulate(elapsed.begin(), elapsed.end(), 0.0) / MeasuredTicks;
    std::ranges::sort(elapsed);
    const double p99 = elapsed[MeasuredTicks * 99 / 100];
    const auto view = simulator.Extract().Value();
    const double checksum = static_cast<double>(view.positionX[0]) + view.positionY[0] + view.positionZ[0];
    std::cout << "particles,modules,warmup_ticks,measured_ticks,mean_ms,p99_ms,max_ms,budget_p99_ms,checksum\n"
              << ParticleCount << ',' << forces.size() << ',' << WarmupTicks << ',' << MeasuredTicks << ',' << std::fixed
              << std::setprecision(6) << mean << ',' << p99 << ',' << elapsed.back() << ',' << BudgetP99Milliseconds << ',' << checksum
              << '\n';
    if (p99 > BudgetP99Milliseconds)
        throw std::runtime_error("Force benchmark exceeds reviewed 60 Hz frame ceiling");
#endif
}
