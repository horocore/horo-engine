#include "Horo/Vfx/GpuParticleSimulator.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"

#include <type_traits>
#include <utility>

namespace {
    template <typename T>
    concept HasHostReadbackOverride = requires(T value) { value.readback; };

    template <typename Source>
    concept CanCreateGpuSimulatorFrom = requires(const Source &source, const Horo::Vfx::GpuParticleSimulatorCreateInfo &info) {
        Horo::Vfx::GpuParticleSimulator::Create(source, info);
    };

    static_assert(CanCreateGpuSimulatorFrom<Horo::Vfx::ParticleCookAdmission>);
    static_assert(!CanCreateGpuSimulatorFrom<Horo::Vfx::ParticleSystemDescriptor>);
    static_assert(!CanCreateGpuSimulatorFrom<Horo::Vfx::ParticleCookPlan>);
    static_assert(!std::is_default_constructible_v<Horo::Vfx::ParticleCookAdmission>);
    static_assert(
        !std::is_constructible_v<Horo::Vfx::ParticleCookAdmission, Horo::Vfx::ParticleSystemDescriptor, Horo::Vfx::ParticleCookPlan>);
    static_assert(std::is_same_v<decltype(std::declval<const Horo::Vfx::ParticleCookAdmission &>().Descriptor()),
                                 const Horo::Vfx::ParticleSystemDescriptor &>);
    static_assert(
        std::is_same_v<decltype(std::declval<const Horo::Vfx::ParticleCookAdmission &>().Plan()), const Horo::Vfx::ParticleCookPlan &>);
    static_assert(!HasHostReadbackOverride<Horo::Vfx::GpuParticleSimulatorCreateInfo>);
    static_assert(std::is_same_v<decltype(Horo::Vfx::ParticleSystemDescriptorData{}.gpuReadback), Horo::Render::VfxGpuReadbackDescriptor>);
    static_assert(std::is_same_v<decltype(Horo::Vfx::ParticleCookPlan{}.gpuReadback), Horo::Render::VfxGpuReadbackDescriptor>);
    static_assert(noexcept(Horo::Render::IsValidVfxGpuReadbackPolicy({}, 0)));
}  // namespace
