#pragma once
/** @file EditorVulkanShaderAssets.h
 * @brief Bounded loading of the packaged, offline-qualified Vulkan viewport bundle. */
#include "EditorVulkanViewportShader.h"

#include <filesystem>

namespace Horo::Editor {
    /** @brief Reads the exact payload/metadata pair under the host's packaged asset root; never invokes a compiler. */
    [[nodiscard]] Result<EditorVulkanViewportCook> LoadPackagedVulkanViewportShader(const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
