#include "VulkanAndroidRuntime.h"

#include <algorithm>
#include <bit>

namespace Horo::Render::AndroidVulkan {
    /** @brief Loads the closed Vulkan 1.1 and maintenance dispatch, never optional desktop 1.3 operations. */
    Result<void> Runtime::LoadDispatch() {
        const auto instance = [this]<class T>(const char *name) {
            return std::bit_cast<T>(vk_.instanceProc(instance_, name));
        };
        const auto device = [this]<class T>(const char *name) {
            return std::bit_cast<T>(vk_.deviceProc(device_, name));
        };
        vk_.createSurface = instance.operator()<PFN_vkCreateAndroidSurfaceKHR>("vkCreateAndroidSurfaceKHR");
        vk_.destroySurface = instance.operator()<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
        vk_.surfaceSupport = instance.operator()<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR");
        vk_.surfaceCapabilities =
            instance.operator()<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        vk_.surfaceFormats = instance.operator()<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
        vk_.surfaceModes = instance.operator()<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
        vk_.createSwapchain = device.operator()<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
        vk_.destroySwapchain = device.operator()<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
        vk_.swapchainImages = device.operator()<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
        vk_.acquire = device.operator()<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
        vk_.present = device.operator()<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
        vk_.releaseImages = device.operator()<PFN_vkReleaseSwapchainImagesEXT>("vkReleaseSwapchainImagesEXT");
        vk_.createView = device.operator()<PFN_vkCreateImageView>("vkCreateImageView");
        vk_.destroyView = device.operator()<PFN_vkDestroyImageView>("vkDestroyImageView");
        vk_.createRenderPass = device.operator()<PFN_vkCreateRenderPass>("vkCreateRenderPass");
        vk_.destroyRenderPass = device.operator()<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        vk_.createFramebuffer = device.operator()<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
        vk_.destroyFramebuffer = device.operator()<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        vk_.createFence = device.operator()<PFN_vkCreateFence>("vkCreateFence");
        vk_.destroyFence = device.operator()<PFN_vkDestroyFence>("vkDestroyFence");
        vk_.fenceStatus = device.operator()<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        vk_.resetFences = device.operator()<PFN_vkResetFences>("vkResetFences");
        vk_.createSemaphore = device.operator()<PFN_vkCreateSemaphore>("vkCreateSemaphore");
        vk_.destroySemaphore = device.operator()<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        vk_.createPool = device.operator()<PFN_vkCreateCommandPool>("vkCreateCommandPool");
        vk_.destroyPool = device.operator()<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
        vk_.allocateCommands = device.operator()<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
        vk_.resetCommand = device.operator()<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
        vk_.beginCommand = device.operator()<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        vk_.endCommand = device.operator()<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        vk_.beginPass = device.operator()<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
        vk_.endPass = device.operator()<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
        vk_.submit = device.operator()<PFN_vkQueueSubmit>("vkQueueSubmit");
        const std::array available{bool(vk_.createSurface),
                                   bool(vk_.destroySurface),
                                   bool(vk_.surfaceSupport),
                                   bool(vk_.surfaceCapabilities),
                                   bool(vk_.surfaceFormats),
                                   bool(vk_.surfaceModes),
                                   bool(vk_.createSwapchain),
                                   bool(vk_.destroySwapchain),
                                   bool(vk_.swapchainImages),
                                   bool(vk_.acquire),
                                   bool(vk_.present),
                                   bool(vk_.releaseImages),
                                   bool(vk_.createView),
                                   bool(vk_.destroyView),
                                   bool(vk_.createRenderPass),
                                   bool(vk_.destroyRenderPass),
                                   bool(vk_.createFramebuffer),
                                   bool(vk_.destroyFramebuffer),
                                   bool(vk_.createFence),
                                   bool(vk_.destroyFence),
                                   bool(vk_.fenceStatus),
                                   bool(vk_.resetFences),
                                   bool(vk_.createSemaphore),
                                   bool(vk_.destroySemaphore),
                                   bool(vk_.createPool),
                                   bool(vk_.destroyPool),
                                   bool(vk_.allocateCommands),
                                   bool(vk_.resetCommand),
                                   bool(vk_.beginCommand),
                                   bool(vk_.endCommand),
                                   bool(vk_.beginPass),
                                   bool(vk_.endPass),
                                   bool(vk_.submit)};
        if (!std::ranges::all_of(available, [](bool value) {
            return value;
        }))
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        return Result<void>::Success();
    }

    /** @brief Acquires bounded unsignaled fences and reusable command buffers before any surface is observed. */
    Result<void> Runtime::CreateSlots() {
        const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                           .queueFamilyIndex = family_};
        auto result = vk_.createPool(device_, &pool, nullptr, &pool_);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateCommandPool", result));
        slotCount_ = config_.maxFramesInFlight;
        for (auto &slot : std::span{slots_}.first(slotCount_)) {
            const VkCommandBufferAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                         .commandPool = pool_,
                                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                         .commandBufferCount = 1};
            result = vk_.allocateCommands(device_, &allocation, &slot.command);
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkAllocateCommandBuffers", result));
            const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            result = vk_.createFence(device_, &fence, nullptr, &slot.acquireFence);
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkCreateFence(acquire)", result));
            result = vk_.createFence(device_, &fence, nullptr, &slot.commandFence);
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkCreateFence(command)", result));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::AndroidVulkan
