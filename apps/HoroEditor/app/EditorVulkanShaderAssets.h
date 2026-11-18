#pragma once
/** @file EditorVulkanShaderAssets.h
 * @brief Bounded loading of the packaged, offline-qualified Vulkan viewport bundle. */
#include "EditorVulkanViewportShader.h"

#include <filesystem>

namespace Horo::Editor {
    /** @brief Reads the exact payload/metadata pair under the host's packaged asset root; never invokes a compiler. */
    [[nodiscard]] Result<EditorVulkanViewportCook> LoadPackagedVulkanViewportShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the exact canonical XR multipass pair only for explicit selected XR startup.
     * @param assetRoot Same host-selected packaged root used by ordinary editor assets.
     * @return Strict same-compiler payload/reflection value or a typed missing/altered/interface failure; never a runtime compiler
     * fallback.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewShader(const std::filesystem::path &assetRoot);
    /** @brief Loads the separately cooked optional XR motion/depth program.
     * @param assetRoot Same selected packaged root as the
     * projection shader.
     * @return The exact motion program or a typed missing, altered or foreign-program error.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewMotionShader(const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
