#pragma once
#include "Horo/Runtime/Render/VfxGpuSimulation.h"

#include <array>

namespace Horo::Render::VfxGpuDetail {
    [[nodiscard]] bool ValidIdentity(const VfxGpuSimulationIdentity &identity) noexcept;
    [[nodiscard]] bool ValidDescriptor(const VfxGpuSimulationDescriptor &descriptor) noexcept;
    [[nodiscard]] bool SameEnvelope(const VfxGpuArithmeticEnvelope &left, const VfxGpuArithmeticEnvelope &right) noexcept;
    [[nodiscard]] bool DecodeForceWords(
        const VfxGpuSimulationPreparation &preparation,
        std::array<Vfx::CpuParticleForceModule, Vfx::ParticleSimulationDataLimits::ForceModules> &forces) noexcept;
}  // namespace Horo::Render::VfxGpuDetail
