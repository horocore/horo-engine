#pragma once

#include "VulkanBackendModule.h"

#include <memory>

namespace Horo::Render::Detail {
    /** @brief Derives one stable backend-scoped identity from a Vulkan device UUID. */
    [[nodiscard]] std::string VulkanAdapterIdentity(const VkPhysicalDeviceIDProperties &identity);
    /** @brief Fingerprints canonical device and driver facts for stale-selection checks. */
    [[nodiscard]] std::uint64_t VulkanAdapterRevision(const std::vector<VulkanAdapterCandidate> &candidates);
    /** @brief Creates the backend-owned Vulkan native runtime over a borrowed platform loader port. */
    [[nodiscard]] std::shared_ptr<IVulkanRuntimePort> CreateVulkanNativeRuntime(IVulkanLoaderPort &loaderPort);
}  // namespace Horo::Render::Detail
