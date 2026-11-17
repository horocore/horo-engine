#pragma once
/** @file VulkanAndroidBackendModule.h @brief Noninstalled explicit Android Vulkan presentation composition. */
#include "Horo/Runtime/Render/RenderBackendRegistry.h"

namespace Horo::Platform::Android {
    class AndroidWindowHost;
}

namespace Horo::Render {
    /** @brief Describes the inert Android Vulkan 1.1 development presentation provider. @return Native-free module policy. */
    [[nodiscard]] RenderBackendModuleInfo GetVulkanAndroidBackendModuleInfo();
    /** @brief Classifies only exact sibling backpressure; native failures remain failures.
     * @param error Original operation error. @return True only for this backend's pending domain/code pair.
     */
    [[nodiscard]] bool IsVulkanAndroidBackpressure(const Error &error) noexcept;
    /** @brief Registers an inert provider; window owner must outlive backend drain and destruction.
     * @param registry Explicit composition registry. @param windows Sole retained Android window owner.
     * @return Registration outcome. No loader, device or native window is acquired here.
     */
    [[nodiscard]] Result<void> RegisterVulkanAndroidRenderBackend(RenderBackendRegistry &registry,
                                                                  Platform::Android::AndroidWindowHost &windows);
}  // namespace Horo::Render
