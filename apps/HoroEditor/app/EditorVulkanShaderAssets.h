#pragma once
/** @file EditorVulkanShaderAssets.h
 * @brief Bounded loading of the packaged, offline-qualified Vulkan viewport bundle. */
#include "EditorVulkanViewportShader.h"

#include <filesystem>

namespace Horo::Editor {
    /** @brief Reads the exact payload/metadata pair under the host's packaged asset root; never invokes a compiler. */
    [[nodiscard]] Result<EditorVulkanViewportCook> LoadPackagedVulkanViewportShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the explicit opaque camera compositor from the installed Vulkan shader root. */
    [[nodiscard]] Result<EditorVulkanCameraCrossFadeCook> LoadPackagedVulkanCameraCrossFadeShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the explicit World canvas shader from the selected installed Vulkan shader root.
     * @param assetRoot Exact host-selected shader asset root.
     * @return Verified World canvas payload and reflection or typed package failure. */
    [[nodiscard]] Result<EditorVulkanWorldCanvasCook> LoadPackagedVulkanWorldCanvasShader(const std::filesystem::path &assetRoot);
    /** @brief Explicitly loads the separate FP16 opaque scene raster pair from the installed asset root.
     * @param assetRoot Host-selected installed shader root.
     * @return Exact scene-linear recipe or typed missing, altered or foreign-program failure.
     */
    [[nodiscard]] Result<EditorVulkanSceneLinearCook> LoadPackagedVulkanSceneLinearShader(const std::filesystem::path &assetRoot);

    /** @brief Loads the exact canonical XR multipass pair only for explicit selected XR startup.
     * @param assetRoot Same host-selected packaged root used by ordinary editor assets.
     * @return Strict same-compiler payload/reflection value or a typed missing/altered/interface failure; never a runtime compiler
     * fallback.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewShader(const std::filesystem::path &assetRoot);

    /** @brief Complete offline-cooked XR program and the exact renderer contract it satisfies. */
    struct PackagedVulkanXrProjection final {
        EditorVulkanMultiviewCook cook;
        EditorVulkanMultiviewProgram program;
    };

    /** @brief Selects the packaged environment projection when declared by its artifact pair.
     * @param assetRoot Host-selected packaged assets.
     * @return Verified program, or a typed package failure. An absent optional pair selects ordinary projection.
     * @details An incomplete or foreign optional pair is an installation failure. Selection grants no sensor access.
     */
    [[nodiscard]] Result<PackagedVulkanXrProjection> LoadPackagedVulkanXrProjection(const std::filesystem::path &assetRoot);
    /** @brief Loads the separately cooked optional XR motion/depth program.
     * @param assetRoot Same selected packaged root as the
     * projection shader.
     * @return The exact motion program or a typed missing, altered or foreign-program error.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewMotionShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the optional Vulkan stereo depth coverage program and its exact reflected sidecar. */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewCoverageShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the optional stereo counter reducer only from the exact installed payload and reflection pair. */
    [[nodiscard]] Result<EditorVulkanCoverageReductionCook> LoadPackagedVulkanCoverageReductionShader(
        const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
