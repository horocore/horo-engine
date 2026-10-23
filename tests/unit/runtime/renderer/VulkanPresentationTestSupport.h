#pragma once

#include "VulkanCommandTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanPresentationRuntime.h"

#include <map>

namespace Horo::Render::VulkanPresentationTest {
    using VulkanTest::EraseFunction;
    using VulkanTest::NativeHandle;

    struct State {
        std::uintptr_t next{100};
        std::map<VkFence, bool> signaled;
        std::map<VkFence, bool> pending;
        VkFence lastAcquire{};
        VkFence lastPresent{};
        VkSurfaceFormatKHR outputFormat{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        VkFormat selectedFormat{};
        VkResult acquireResult{VK_SUCCESS};
        VkResult presentResult{VK_SUCCESS};
        bool gpuComplete{};
        bool invalidDestroy{};
        bool validAcquire{true};
        bool validPresent{true};
        int acquires{};
        int releases{};
        int presents{};
        int chainsCreated{};
        int chainsDestroyed{};
        int blockingLookups{};
        std::vector<std::uint32_t> released;

        void CompleteAll() {
            gpuComplete = true;
            for (auto &[fence, value] : signaled)
                value = true;
        }
    };

    inline thread_local State *native{};

    inline VKAPI_ATTR VkResult VKAPI_CALL Capabilities(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *out) {
        *out = {.minImageCount = 2,
                .maxImageCount = 4,
                .currentExtent = {UINT32_MAX, UINT32_MAX},
                .minImageExtent = {1, 1},
                .maxImageExtent = {4096, 4096},
                .maxImageArrayLayers = 1,
                .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Formats(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t *count, VkSurfaceFormatKHR *out) {
        *count = 1;
        if (out)
            *out = native->outputFormat;
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Modes(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t *count, VkPresentModeKHR *out) {
        *count = 2;
        if (out) {
            out[0] = VK_PRESENT_MODE_FIFO_KHR;
            out[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL CreateChain(VkDevice, const VkSwapchainCreateInfoKHR *request, const VkAllocationCallbacks *,
                                                      VkSwapchainKHR *out) {
        ++native->chainsCreated;
        native->selectedFormat = request->imageFormat;
        *out = NativeHandle<VkSwapchainKHR>(native->next++);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroyChain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *) {
        ++native->chainsDestroyed;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Images(VkDevice, VkSwapchainKHR, std::uint32_t *count, VkImage *out) {
        *count = 3;
        if (out)
            for (std::uint32_t index = 0; index < *count; ++index)
                out[index] = NativeHandle<VkImage>(1000 + index);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *,
                                                     VkImageView *out) {
        *out = NativeHandle<VkImageView>(native->next++);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroyView(VkDevice, VkImageView, const VkAllocationCallbacks *) {}

    inline VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
                                                          VkSemaphore *out) {
        *out = NativeHandle<VkSemaphore>(native->next++);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {}

    inline VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *out) {
        *out = NativeHandle<VkFence>(native->next++);
        native->signaled[*out] = false;
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, const VkFence fence, const VkAllocationCallbacks *) {
        native->invalidDestroy |= native->pending[fence] && !native->signaled[fence];
        native->signaled.erase(fence);
        native->pending.erase(fence);
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL FenceStatus(VkDevice, const VkFence fence) {
        return native->signaled[fence] ? VK_SUCCESS : VK_NOT_READY;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, const std::uint32_t count, const VkFence *fences) {
        for (std::uint32_t index = 0; index < count; ++index) {
            native->invalidDestroy |= native->pending[fences[index]] && !native->signaled[fences[index]];
            native->signaled[fences[index]] = false;
            native->pending[fences[index]] = false;
        }
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice, VkSwapchainKHR, const std::uint64_t timeout, const VkSemaphore semaphore,
                                                  const VkFence fence, std::uint32_t *image) {
        ++native->acquires;
        native->validAcquire &= timeout == 0 && semaphore == VK_NULL_HANDLE && !native->signaled[fence];
        if (native->acquireResult == VK_SUCCESS || native->acquireResult == VK_SUBOPTIMAL_KHR) {
            native->lastAcquire = fence;
            native->pending[fence] = true;
            *image = 0;
        }
        return native->acquireResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue, const VkPresentInfoKHR *request) {
        ++native->presents;
        const auto fence = std::bit_cast<const VkSwapchainPresentFenceInfoEXT *>(request->pNext);
        native->validPresent &= fence && fence->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT && fence->swapchainCount == 1 &&
                                request->waitSemaphoreCount == 1;
        if (native->presentResult != VK_ERROR_OUT_OF_HOST_MEMORY && native->presentResult != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            native->presentResult != VK_ERROR_DEVICE_LOST) {
            native->lastPresent = fence->pFences[0];
            native->pending[native->lastPresent] = true;
        }
        return native->presentResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Release(VkDevice, const VkReleaseSwapchainImagesInfoEXT *request) {
        ++native->releases;
        native->released.push_back(request->pImageIndices[0]);
        return VK_SUCCESS;
    }

    inline PFN_vkVoidFunction Resolve(const char *name) {
        const std::array functions{std::pair{"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", EraseFunction(Capabilities)},
                                   std::pair{"vkGetPhysicalDeviceSurfaceFormatsKHR", EraseFunction(Formats)},
                                   std::pair{"vkGetPhysicalDeviceSurfacePresentModesKHR", EraseFunction(Modes)},
                                   std::pair{"vkCreateSwapchainKHR", EraseFunction(CreateChain)},
                                   std::pair{"vkDestroySwapchainKHR", EraseFunction(DestroyChain)},
                                   std::pair{"vkGetSwapchainImagesKHR", EraseFunction(Images)},
                                   std::pair{"vkAcquireNextImageKHR", EraseFunction(Acquire)},
                                   std::pair{"vkQueuePresentKHR", EraseFunction(Present)},
                                   std::pair{"vkReleaseSwapchainImagesEXT", EraseFunction(Release)},
                                   std::pair{"vkCreateImageView", EraseFunction(CreateView)},
                                   std::pair{"vkDestroyImageView", EraseFunction(DestroyView)},
                                   std::pair{"vkCreateSemaphore", EraseFunction(CreateSemaphore)},
                                   std::pair{"vkDestroySemaphore", EraseFunction(DestroySemaphore)},
                                   std::pair{"vkCreateFence", EraseFunction(CreateFence)},
                                   std::pair{"vkDestroyFence", EraseFunction(DestroyFence)},
                                   std::pair{"vkGetFenceStatus", EraseFunction(FenceStatus)},
                                   std::pair{"vkResetFences", EraseFunction(ResetFences)}};
        if (std::string_view{name} == "vkWaitForFences" || std::string_view{name} == "vkQueueWaitIdle" ||
            std::string_view{name} == "vkDeviceWaitIdle")
            ++native->blockingLookups;
        const auto found = std::ranges::find_if(functions, [&](const auto &entry) {
            return std::string_view{entry.first} == name;
        });
        return found == functions.end() ? nullptr : found->second;
    }

    inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceResolver(VkInstance, const char *name) {
        return Resolve(name);
    }

    inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceResolver(VkDevice, const char *name) {
        return Resolve(name);
    }

    struct Fixture {
        State state;
        std::function<Result<bool>(RenderTimelinePoint)> observe{[this](RenderTimelinePoint) {
            return Result<bool>::Success(state.gpuComplete);
        }};
        Detail::VulkanPresentationRuntime runtime;

        Fixture() {
            native = &state;
            REQUIRE(runtime
                        .Initialize({NativeHandle<VkInstance>(1), NativeHandle<VkPhysicalDevice>(2), NativeHandle<VkDevice>(3),
                                     NativeHandle<VkSurfaceKHR>(4), NativeHandle<VkQueue>(5), 0, 0, 2, InstanceResolver, DeviceResolver,
                                     observe})
                        .HasValue());
        }

        ~Fixture() {
            state.CompleteAll();
            Test::RequireDrainedOwner(runtime);
            native = nullptr;
        }

        void Realize() {
            REQUIRE(runtime.Resize({640, 480}, PresentMode::Fifo).HasValue());
        }

        void AcquireReady() {
            REQUIRE_FALSE(runtime.PollAvailability({640, 480}).Value());
            state.signaled[state.lastAcquire] = true;
            REQUIRE(runtime.PollAvailability({640, 480}).Value());
        }
    };
}  // namespace Horo::Render::VulkanPresentationTest
