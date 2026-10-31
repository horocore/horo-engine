#pragma once
/** @file VulkanMultiviewDepthSupport.h @brief Private initialization-time admission of complete multiview depth storage. */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>

namespace Horo::Render::Detail {
    /** @brief Intersection of implemented complete views and queried depth-array storage; zero means unavailable. */
    struct VulkanMultiviewDepthLimits final {
        std::uint32_t maximumViews{};
        std::uint32_t maximumExtent2D{};
    };

    /** @brief Cold native query callable; the Vulkan entry point retains its platform calling convention. */
    using VulkanDepthFormatQuery = std::function<VkResult(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags,
                                                          VkImageCreateFlags, VkImageFormatProperties *)>;

    /** @brief Queries the exact D32/2D/optimal/depth-attachment envelope once during backend composition.
     * @param device Selected physical device.
     * @param query Actual instance entry point; missing dispatch is a typed failure.
     * @param implementedViews Existing finite complete-view implementation bound.
     * @param limits Queried physical-device limits, never host policy or inferred API-name support.
     * @return Bounded array/extent facts, unavailable for an unsupported tuple, or the original native-query failure.
     */
    [[nodiscard]] Result<VulkanMultiviewDepthLimits> QueryMultiviewDepthLimits(VkPhysicalDevice device, const VulkanDepthFormatQuery &query,
                                                                               std::uint32_t implementedViews,
                                                                               const VkPhysicalDeviceLimits &limits);
}  // namespace Horo::Render::Detail
