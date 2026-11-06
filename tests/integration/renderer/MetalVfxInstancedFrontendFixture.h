#pragma once
/** @file MetalVfxInstancedFrontendFixture.h @brief Real public frontend indexed VFX qualification; no simulated native evidence. */
#include <filesystem>

namespace Horo::Render::Test {
    /** @brief Renders the actual off-axis seven-particle scene and checks pixels, depth and retained-slot lifetime. */
    void RunMetalVfxInstancedPixels(const std::filesystem::path &root);
    /** @brief Uses an actual two-texel masked shader to distinguish discarded depth from visible depth writing. */
    void RunMetalVfxInstancedMaskedDepth(const std::filesystem::path &root);
    /** @brief Encodes then aborts an unsent real VFX graph and proves subsequent owner admission remains usable. */
    void RunMetalVfxInstancedAbort(const std::filesystem::path &root);
    /** @brief Reports measured cold/hot reference work without inventing an unapproved throughput threshold. */
    void RunMetalVfxInstancedReference(const std::filesystem::path &root);
}  // namespace Horo::Render::Test
