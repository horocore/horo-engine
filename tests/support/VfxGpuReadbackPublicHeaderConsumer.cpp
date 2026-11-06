#include "Horo/Vfx/GpuParticleSimulator.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"

#include <type_traits>

namespace {
    template <typename T>
    concept HasHostReadbackOverride = requires(T value) { value.readback; };

    static_assert(!HasHostReadbackOverride<Horo::Vfx::GpuParticleSimulatorCreateInfo>);
    static_assert(std::is_same_v<decltype(Horo::Vfx::ParticleSystemDescriptorData{}.gpuReadback), Horo::Render::VfxGpuReadbackDescriptor>);
    static_assert(std::is_same_v<decltype(Horo::Vfx::ParticleCookPlan{}.gpuReadback), Horo::Render::VfxGpuReadbackDescriptor>);
    static_assert(noexcept(Horo::Render::IsValidVfxGpuReadbackPolicy({}, 0)));
}  // namespace
