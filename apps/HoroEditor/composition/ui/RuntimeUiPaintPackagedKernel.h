#pragma once
/** @file RuntimeUiPaintPackagedKernel.h @brief Explicit application asset-root loading; never a runtime compiler. */
#include "RuntimeUiPaintShaderPackage.h"

#include <filesystem>

namespace Horo::Application::Internal {
    /** @brief Loads the exact packaged Metal UI payload/record pair at the Scene preparation safe point.
     * @param assetRoot Explicit longer-lived application packaged asset root. @param cancellation Actual host ancestry.
     * @param selectedBackend Actual host-selected backend; no fallback or backend discovery.
     * @return Owned strict kernel or typed missing, malformed, cancelled or identity failure.
     * @details The host keeps the asset root quiescent during this bounded read. Symlink/path escapes, nonregular files,
     * growth and truncation reject before admission. No source request means no call or filesystem work. */
    [[nodiscard]] Result<std::shared_ptr<const Render::CookedUiPaintKernel>> LoadPackagedProjectUiMetalKernel(
        const std::filesystem::path &assetRoot, Render::ShaderTargetBackend selectedBackend, const CancellationToken &cancellation);
    /** @brief Loads the exact packaged Vulkan paint kernel for a selected scene canvas.
     * @param assetRoot Explicit admitted application asset root. @param selectedBackend Actual selected backend.
     * @param cancellation Current load operation ancestry.
     * @return Finite verified offscreen kernel or original typed preparation error.
     * @details Caller retains package admission. The load does not select a backend or run a compiler. */
    [[nodiscard]] Result<std::shared_ptr<const Render::CookedUiPaintKernel>> LoadPackagedProjectUiVulkanSceneKernel(
        const std::filesystem::path &assetRoot, Render::ShaderTargetBackend selectedBackend, const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
