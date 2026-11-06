#pragma once
#include "Horo/Vfx/GpuParticleSimulator.h"

#include <optional>
#include <thread>
#include <vector>

namespace Horo::Vfx::Detail {
    /** @brief One bounded accepted native step; completion releases logical capacity only, never native resources. */
    struct GpuParticlePending final {
        Render::VfxGpuStepIdentity step;
        Render::RenderTimelinePoint submission;
        bool occupied{};
    };

    /** @brief Scene-owner mutable logical state; renderer retains all independent native resource and submission leases. */
    struct GpuParticleSimulatorState final {
        Render::VfxGpuSimulationPreparation preparation;
        std::vector<std::uint32_t> initialWords, forceWords;
        std::vector<GpuParticlePending> submitted;
        std::optional<Render::GpuVfxSimulationWork> pending;
        Render::VfxGpuPreparationReceipt receipt;
        std::thread::id owner{std::this_thread::get_id()};
        CancellationSource lifetimeSource;
        CancellationToken lifetimeToken{lifetimeSource.Token()};
        std::uint64_t nextTick{};
        bool active{}, stopped{}, shutdown{}, uncertain{}, deviceLost{};
    };
}  // namespace Horo::Vfx::Detail

namespace Horo::Vfx::GpuParticleDetail {
    [[nodiscard]] Result<std::unique_ptr<Detail::GpuParticleSimulatorState>> PrepareGpuState(const ParticleCookAdmission &admission,
                                                                                             const GpuParticleSimulatorCreateInfo &info);
}
