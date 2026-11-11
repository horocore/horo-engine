#pragma once
/** @file MetalVfxGpuEndToEndFixture.h @brief Genuine public GPU VFX vertical-slice qualification, never a runtime implementation. */
#include <filesystem>

namespace Horo::Render::Test {
    enum class VfxGpuObservationCase {
        LeaseBackpressure,
        Cancelled,
        TimedOut
    };
    void RunDisabledMetalVfxGpu(const std::filesystem::path &);
    void RunEquivalentMetalVfxGpu(const std::filesystem::path &);
    void RunObservedMetalVfxGpu(const std::filesystem::path &, VfxGpuObservationCase);
}  // namespace Horo::Render::Test
