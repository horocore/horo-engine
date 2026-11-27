#pragma once
/** @file MetalVfxGpuEndToEndFixture.h @brief Genuine public GPU VFX vertical-slice qualification, never a runtime implementation. */
#include <filesystem>

namespace Horo::Render::Test {
    enum class VfxGpuObservationCase {
        LeaseBackpressure,
        Cancelled,
        TimedOut
    };
    /** @brief Checks the real 65,536-particle Metal compute/draw wall-time budget with observations disabled.
     * @param root Genuine cooked simulation/direct-draw package root.
     * @details Reports native device identity and timing distribution. Missing native compute visibly skips;
     * invalid packages or an exceeded budget fail the test.
     */
    void RunBudgetMetalVfxGpu(const std::filesystem::path &root);
    void RunDisabledMetalVfxGpu(const std::filesystem::path &);
    /** @brief Compares twelve native simple-force steps against the independent versioned CPU reference.
     * @param root Genuine cooked simulation/direct-draw package root.
     * @details Checks individual forces and their composition using bounded authored observations. Missing
     * native compute visibly skips; invalid packages or observations outside tolerance fail the test.
     */
    void RunEquivalentMetalVfxGpu(const std::filesystem::path &root);
    void RunObservedMetalVfxGpu(const std::filesystem::path &, VfxGpuObservationCase);
}  // namespace Horo::Render::Test
