#pragma once
/** @file MetalVfxInstancedFrontendFixture.h @brief Real public frontend indexed VFX qualification; no simulated native evidence. */
#include <cstdint>
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
    /** @brief Runs one explicitly requested static-scene native measurement iteration; no throughput threshold is enforced.
     * @param root Authentic ordinary/masked offline package. @param instances One of 1024,4096,16384.
     * @param receipt New exclusive JSONL destination; bounded raw observations are written after the steady window.
     * @details Dedicated manual executable only. CPU metrics exclude acquisition/completion/extraction; GPU time is the whole
     * actual command buffer. Serial completion observation and requested validation-off form a separate diagnostic cohort.
     */
    void RunMetalVfxInstancedBenchmark(const std::filesystem::path &root, std::uint32_t instances, const std::filesystem::path &receipt);
    /** @brief Measures a separately named static opaque cube count cohort, without simulation or engine parity claims.
     * @param root Existing authentic instanced-shader package. @param instances Exactly 5000 or 90000.
     * @param receipt New absolute exclusive JSONL destination. @details 1600x900, fixed 0.04 world-size 12-triangle cubes,
     * one opaque CPU-world emitter, canonical indexed chunks and unchanged bounded Fifo measurement protocol.
     */
    void RunMetalVfxInstancedOpaqueCubeScalability(const std::filesystem::path &root, std::uint32_t instances,
                                                   const std::filesystem::path &receipt);
    /** @brief Repeats a real bounded submission-pin rejection and checks unsent diagnostic cleanup without measurement. */
    void RunMetalVfxInstancedPinBudgetRejection(const std::filesystem::path &root);
    /** @brief Measures the canonical finite CPU World simulation and rebuilt native additive billboard graph; static profiles unchanged. */
    void RunMetalVfxInstancedDynamicBenchmark(const std::filesystem::path &root, std::uint32_t instances,
                                              const std::filesystem::path &receipt);
    /** @brief Proves bounded multi-chunk dynamic reuse and genuine simulation/native uniform progress outside long measurements. */
    void RunMetalVfxInstancedDynamicReuse(const std::filesystem::path &root);
}  // namespace Horo::Render::Test
