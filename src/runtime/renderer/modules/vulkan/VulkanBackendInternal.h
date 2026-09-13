#pragma once

#include "VulkanBackendModule.h"

#include <algorithm>
#include <string_view>

namespace Horo::Render::Detail {
    inline constexpr VulkanApiVersion RequiredVulkanApiVersion{1, 3, 0};

    [[nodiscard]] inline bool VulkanSupportsVersion(const VulkanApiVersion version) noexcept {
        return version.major > RequiredVulkanApiVersion.major ||
               (version.major == RequiredVulkanApiVersion.major && version.minor >= RequiredVulkanApiVersion.minor);
    }

    [[nodiscard]] inline bool VulkanLoaderMeetsBaseline(const VulkanLoaderCapabilities &loader) noexcept {
        return loader.hasInstanceVersionQuery && VulkanSupportsVersion(loader.apiVersion);
    }

    [[nodiscard]] inline bool VulkanContains(const std::vector<std::string> &values, const std::string_view expected) {
        return std::ranges::find(values, expected) != values.end();
    }

    [[nodiscard]] inline bool VulkanHasQueue(const VulkanAdapterCandidate &candidate, const bool presentation) {
        return std::ranges::any_of(candidate.queueFamilies, [presentation](const VulkanQueueFamily &queue) {
            return presentation ? queue.supportsPresentation : queue.supportsGraphics;
        });
    }

    /** @brief Test seam that shares registration implementation with host composition. */
    [[nodiscard]] Result<void> RegisterVulkanRenderBackendWithRuntimePort(RenderBackendRegistry &registry, IVulkanRuntimePort &runtimePort);
    /** @brief Registration seam that retains one backend-owned native runtime. */
    [[nodiscard]] Result<void> RegisterVulkanRenderBackendWithOwnedRuntimePort(RenderBackendRegistry &registry,
                                                                               std::shared_ptr<IVulkanRuntimePort> runtimePort);
    /** @brief Test seam that creates discovery over a borrowed backend-native runtime. */
    [[nodiscard]] std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithRuntimePort(IVulkanRuntimePort &runtimePort);
    /** @brief Creates discovery while retaining one backend-owned native runtime. */
    [[nodiscard]] std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithOwnedRuntimePort(
        std::shared_ptr<IVulkanRuntimePort> runtimePort);
}  // namespace Horo::Render::Detail
