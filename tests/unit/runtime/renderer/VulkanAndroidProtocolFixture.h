#pragma once
/** @file VulkanAndroidProtocolFixture.h @brief Fake native dispatch exercising actual sibling ownership; never device qualification. */
#include "Horo/Platform/Internal/AndroidLifecycleAdapter.h"
#include "VulkanAndroidRuntime.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <type_traits>

namespace Horo::Render::AndroidVulkan::Testing {
    /** @brief Extracts real protocol outcomes with failure context. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Accepted"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Requires a successful real protocol operation. */
    inline void Accepted(const Result<void> &result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Accepted"));
        REQUIRE(result.HasValue());
    }

    /** @brief Constructs fake native tokens without claiming physical objects. */
    template <class T> T Handle(std::uint64_t value) {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    /** @brief Explicit fake Vulkan callbacks and independent acquisition/render/presentation completion switches. */
    struct Driver final {
        inline static Driver *current{};
        std::uint64_t next{1};
        enum class Kind {
            None,
            Acquire,
            Render,
            Present
        };
        std::array<VkFence, 256> fences{};
        std::array<Kind, 256> kinds{};
        std::size_t fenceCount{};
        bool maintenance{true}, acquireReady{true}, renderReady{true}, presentReady{true}, surfaceSupported{true}, failSwapchain{};
        std::size_t instanceCreates{}, deviceCreates{}, surfaceCreates{}, surfaceDestroys{}, chainDestroys{}, submits{}, presents{},
            releases{}, passes{}, acquisitions{}, chainCreates{}, commandBegins{};
        VkCompositeAlphaFlagsKHR supportedAlpha{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR};
        VkCompositeAlphaFlagBitsKHR chosenAlpha{};
        float recordedClearAlpha{};
        bool enabledMaintenance{}, requested11{};
        std::uint32_t acquiredImage{};
        std::array<std::size_t, 4> waitsByKind{};
        VkResult fenceFailure{VK_SUCCESS};
        VkResult acquireResult{VK_SUCCESS}, presentResult{VK_SUCCESS};

        Driver() {
            current = this;
        }

        /** @brief Issues bounded fake native identities. */
        template <class T> static T Issue() {
            return Handle<T>(current->next++);
        }

        /** @brief Stores one exact observed fake fence's lifecycle class. */
        static void Mark(VkFence fence, Kind kind) {
            const auto found = std::ranges::find(current->fences, fence);
            REQUIRE(found != current->fences.end());
            current->kinds[found - current->fences.begin()] = kind;
        }

        /** @brief Supplies the admitted fake instance inventory. */
        static VkResult VKAPI_CALL InstanceExtensions(const char *, std::uint32_t *count, VkExtensionProperties *out) {
            constexpr std::array names{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                                       VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
            if (out)
                for (std::size_t i = 0; i < names.size(); ++i)
                    std::strcpy(out[i].extensionName, names[i]);
            *count = names.size();
            return VK_SUCCESS;
        }

        /** @brief Records the actual requested API and never creates a fake surface implicitly. */
        static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *, VkInstance *out) {
            ++current->instanceCreates;
            current->requested11 = info->pApplicationInfo->apiVersion == VK_API_VERSION_1_1;
            *out = Issue<VkInstance>();
            return VK_SUCCESS;
        }

        /** @brief Observes instance teardown. */
        static void VKAPI_CALL DestroyInstance(VkInstance, const VkAllocationCallbacks *) {}

        /** @brief Supplies exactly one fake physical device. */
        static VkResult VKAPI_CALL Enumerate(VkInstance, std::uint32_t *count, VkPhysicalDevice *out) {
            if (out)
                *out = Handle<VkPhysicalDevice>(900);
            *count = 1;
            return VK_SUCCESS;
        }

        /** @brief Supplies only the admitted 1.1 version. */
        static void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties *out) {
            out->apiVersion = VK_API_VERSION_1_1;
        }

        /** @brief Supplies the maintenance extension names; feature support remains independently controlled. */
        static VkResult VKAPI_CALL DeviceExtensions(VkPhysicalDevice, const char *, std::uint32_t *count, VkExtensionProperties *out) {
            if (out) {
                std::strcpy(out[0].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                std::strcpy(out[1].extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            }
            *count = 2;
            return VK_SUCCESS;
        }

        /** @brief Answers the actual requested maintenance feature chain. */
        static void VKAPI_CALL Features(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *out) {
            auto *maintenance = static_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT *>(out->pNext);
            maintenance->swapchainMaintenance1 = current->maintenance;
        }

        /** @brief Supplies one actual fake graphics family without fabricating surface support. */
        static void VKAPI_CALL Queues(VkPhysicalDevice, std::uint32_t *count, VkQueueFamilyProperties *out) {
            if (out) {
                out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
                out[0].queueCount = 1;
            }
            *count = 1;
        }

        /** @brief Records actual enabled feature bytes. */
        static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *,
                                                VkDevice *out) {
            ++current->deviceCreates;
            current->enabledMaintenance =
                static_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT *>(info->pNext)->swapchainMaintenance1;
            *out = Issue<VkDevice>();
            return VK_SUCCESS;
        }

        /** @brief Observes device teardown. */
        static void VKAPI_CALL DestroyDevice(VkDevice, const VkAllocationCallbacks *) {}

        /** @brief Supplies the one selected fake queue. */
        static void VKAPI_CALL Queue(VkDevice, std::uint32_t, std::uint32_t, VkQueue *out) {
            *out = Handle<VkQueue>(901);
        }

        /** @brief Records only explicit retained-window surface creation. */
        static VkResult VKAPI_CALL CreateSurface(VkInstance, const VkAndroidSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *,
                                                 VkSurfaceKHR *out) {
            REQUIRE(info->window);
            ++current->surfaceCreates;
            *out = Issue<VkSurfaceKHR>();
            return VK_SUCCESS;
        }

        /** @brief Records generation-local surface destruction. */
        static void VKAPI_CALL DestroySurface(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *) {
            ++current->surfaceDestroys;
        }

        /** @brief Independently admits or rejects the already selected queue against a concrete surface. */
        static VkResult VKAPI_CALL SurfaceSupport(VkPhysicalDevice, std::uint32_t, VkSurfaceKHR, VkBool32 *out) {
            *out = current->surfaceSupported;
            return VK_SUCCESS;
        }

        /** @brief Supplies finite closed fake native capabilities. */
        static VkResult VKAPI_CALL Capabilities(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *out) {
            *out = {.minImageCount = 2,
                    .maxImageCount = 4,
                    .currentExtent = {UINT32_MAX, UINT32_MAX},
                    .minImageExtent = {1, 1},
                    .maxImageExtent = {2048, 2048},
                    .maxImageArrayLayers = 1,
                    .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                    .supportedCompositeAlpha = current->supportedAlpha,
                    .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
            return VK_SUCCESS;
        }

        /** @brief Supplies exact unmanaged SDR surface metadata. */
        static VkResult VKAPI_CALL Formats(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t *count, VkSurfaceFormatKHR *out) {
            if (out)
                *out = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
            *count = 1;
            return VK_SUCCESS;
        }

        /** @brief Supplies only FIFO, without advertising unsupported modes. */
        static VkResult VKAPI_CALL Modes(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t *count, VkPresentModeKHR *out) {
            if (out)
                *out = VK_PRESENT_MODE_FIFO_KHR;
            *count = 1;
            return VK_SUCCESS;
        }

        /** @brief Injects exact cold swapchain failure or supplies one fake chain. */
        static VkResult VKAPI_CALL CreateChain(VkDevice, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *,
                                               VkSwapchainKHR *out) {
            ++current->chainCreates;
            current->chosenAlpha = info->compositeAlpha;
            if (current->failSwapchain)
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            *out = Issue<VkSwapchainKHR>();
            return VK_SUCCESS;
        }

        /** @brief Records chain retirement. */
        static void VKAPI_CALL DestroyChain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *) {
            ++current->chainDestroys;
        }

        /** @brief Supplies the complete fixed image inventory. */
        static VkResult VKAPI_CALL Images(VkDevice, VkSwapchainKHR, std::uint32_t *count, VkImage *out) {
            if (out) {
                out[0] = Handle<VkImage>(902);
                out[1] = Handle<VkImage>(903);
            }
            *count = 2;
            return VK_SUCCESS;
        }

        /** @brief Emulates timeout-zero acquisition; completion is independently observed through its fence. */
        static VkResult VKAPI_CALL Acquire(VkDevice, VkSwapchainKHR, std::uint64_t timeout, VkSemaphore, VkFence fence,
                                           std::uint32_t *out) {
            ++current->acquisitions;
            CHECK(timeout == 0);
            if (current->acquireResult != VK_SUCCESS)
                return current->acquireResult;
            *out = current->acquiredImage;
            Mark(fence, Kind::Acquire);
            return VK_SUCCESS;
        }

        /** @brief Records the exact presentation fence independently of command completion. */
        static VkResult VKAPI_CALL Present(VkQueue, const VkPresentInfoKHR *info) {
            ++current->presents;
            const auto *fence = static_cast<const VkSwapchainPresentFenceInfoEXT *>(info->pNext);
            Mark(*fence->pFences, Kind::Present);
            return current->presentResult;
        }

        /** @brief Records explicit maintenance image returns. */
        static VkResult VKAPI_CALL Release(VkDevice, const VkReleaseSwapchainImagesInfoEXT *) {
            ++current->releases;
            return VK_SUCCESS;
        }

        /** @brief Creates an exact fake image-view token. */
        static VkResult VKAPI_CALL CreateView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *out) {
            *out = Issue<VkImageView>();
            return VK_SUCCESS;
        }

        /** @brief Observes view teardown. */
        static void VKAPI_CALL DestroyView(VkDevice, VkImageView, const VkAllocationCallbacks *) {}

        /** @brief Creates a fake render pass from actual production 1.1 metadata. */
        static VkResult VKAPI_CALL CreatePass(VkDevice, const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *,
                                              VkRenderPass *out) {
            CHECK(info->pAttachments->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
            *out = Issue<VkRenderPass>();
            return VK_SUCCESS;
        }

        /** @brief Observes render-pass teardown. */
        static void VKAPI_CALL DestroyPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *) {}

        /** @brief Creates a fake framebuffer token. */
        static VkResult VKAPI_CALL CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *,
                                                     VkFramebuffer *out) {
            *out = Issue<VkFramebuffer>();
            return VK_SUCCESS;
        }

        /** @brief Observes framebuffer teardown. */
        static void VKAPI_CALL DestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *) {}

        /** @brief Creates bounded initially unsignaled fence state. */
        static VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *out) {
            REQUIRE(current->fenceCount < current->fences.size());
            *out = Issue<VkFence>();
            current->fences[current->fenceCount++] = *out;
            return VK_SUCCESS;
        }

        /** @brief Observes fence teardown. */
        static void VKAPI_CALL DestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *) {}

        /** @brief Validates exact zero-time native observation and returns independent fence state or an injected native failure. */
        static VkResult VKAPI_CALL WaitFences(VkDevice, std::uint32_t count, const VkFence *fences, VkBool32 waitAll,
                                              std::uint64_t timeout) {
            REQUIRE(count == 1);
            REQUIRE(fences);
            CHECK(waitAll == VK_TRUE);
            CHECK(timeout == 0);
            const auto found = std::ranges::find(current->fences, fences[0]);
            REQUIRE(found != current->fences.end());
            const auto kind = current->kinds[found - current->fences.begin()];
            ++current->waitsByKind[static_cast<std::size_t>(kind)];
            if (current->fenceFailure != VK_SUCCESS)
                return current->fenceFailure;
            switch (kind) {
                case Kind::Acquire:
                    return current->acquireReady ? VK_SUCCESS : VK_TIMEOUT;
                case Kind::Render:
                    return current->renderReady ? VK_SUCCESS : VK_TIMEOUT;
                case Kind::Present:
                    return current->presentReady ? VK_SUCCESS : VK_TIMEOUT;
                default:
                    return VK_TIMEOUT;
            }
        }

        /** @brief Resets exact fence state, without simulating completion. */
        static VkResult VKAPI_CALL ResetFences(VkDevice, std::uint32_t count, const VkFence *fences) {
            for (std::uint32_t i = 0; i < count; ++i)
                Mark(fences[i], Kind::None);
            return VK_SUCCESS;
        }

        /** @brief Creates a fake semaphore token. */
        static VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
                                                   VkSemaphore *out) {
            *out = Issue<VkSemaphore>();
            return VK_SUCCESS;
        }

        /** @brief Observes semaphore teardown. */
        static void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {}

        /** @brief Creates a real-protocol command-pool token. */
        static VkResult VKAPI_CALL CreatePool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *,
                                              VkCommandPool *out) {
            *out = Issue<VkCommandPool>();
            return VK_SUCCESS;
        }

        /** @brief Observes pool teardown. */
        static void VKAPI_CALL DestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *) {}

        /** @brief Allocates the exact single requested fake command buffer. */
        static VkResult VKAPI_CALL Commands(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out) {
            *out = Issue<VkCommandBuffer>();
            return VK_SUCCESS;
        }

        /** @brief Accepts reset of an owner-proven retired fake command buffer. */
        static VkResult VKAPI_CALL ResetCommand(VkCommandBuffer, VkCommandBufferResetFlags) {
            return VK_SUCCESS;
        }

        /** @brief Accepts fake command recording. */
        static VkResult VKAPI_CALL BeginCommand(VkCommandBuffer, const VkCommandBufferBeginInfo *) {
            ++current->commandBegins;
            return VK_SUCCESS;
        }

        /** @brief Accepts complete fake command recording. */
        static VkResult VKAPI_CALL EndCommand(VkCommandBuffer) {
            return VK_SUCCESS;
        }

        /** @brief Records actual production begin-render-pass execution, not a GPU pixel result. */
        static void VKAPI_CALL BeginPass(VkCommandBuffer, const VkRenderPassBeginInfo *info, VkSubpassContents) {
            ++current->passes;
            current->recordedClearAlpha = info->pClearValues[0].color.float32[3];
        }

        /** @brief Completes the fake render-pass callback. */
        static void VKAPI_CALL EndPass(VkCommandBuffer) {}

        /** @brief Records accepted submission and tags only its own completion fence. */
        static VkResult VKAPI_CALL Submit(VkQueue, std::uint32_t, const VkSubmitInfo *, VkFence fence) {
            ++current->submits;
            Mark(fence, Kind::Render);
            return VK_SUCCESS;
        }

        /** @brief Converts exact typed fake callbacks into the Vulkan resolver representation. */
        template <class T> static PFN_vkVoidFunction Proc(T function) {
            return std::bit_cast<PFN_vkVoidFunction>(function);
        }

        /** @brief Maps only explicitly defined fake native functions; unknown names never acquire pretend support. */
        static PFN_vkVoidFunction VKAPI_CALL Resolve(VkInstance, const char *name) {
            const std::array table{std::pair{"vkEnumerateInstanceExtensionProperties", Proc(&InstanceExtensions)},
                                   std::pair{"vkCreateInstance", Proc(&CreateInstance)},
                                   std::pair{"vkDestroyInstance", Proc(&DestroyInstance)},
                                   std::pair{"vkEnumeratePhysicalDevices", Proc(&Enumerate)},
                                   std::pair{"vkGetPhysicalDeviceProperties", Proc(&Properties)},
                                   std::pair{"vkEnumerateDeviceExtensionProperties", Proc(&DeviceExtensions)},
                                   std::pair{"vkGetPhysicalDeviceFeatures2", Proc(&Features)},
                                   std::pair{"vkGetPhysicalDeviceQueueFamilyProperties", Proc(&Queues)},
                                   std::pair{"vkCreateDevice", Proc(&CreateDevice)},
                                   std::pair{"vkGetDeviceProcAddr", Proc(&ResolveDevice)},
                                   std::pair{"vkDestroyDevice", Proc(&DestroyDevice)},
                                   std::pair{"vkGetDeviceQueue", Proc(&Queue)},
                                   std::pair{"vkCreateAndroidSurfaceKHR", Proc(&CreateSurface)},
                                   std::pair{"vkDestroySurfaceKHR", Proc(&DestroySurface)},
                                   std::pair{"vkGetPhysicalDeviceSurfaceSupportKHR", Proc(&SurfaceSupport)},
                                   std::pair{"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", Proc(&Capabilities)},
                                   std::pair{"vkGetPhysicalDeviceSurfaceFormatsKHR", Proc(&Formats)},
                                   std::pair{"vkGetPhysicalDeviceSurfacePresentModesKHR", Proc(&Modes)},
                                   std::pair{"vkCreateSwapchainKHR", Proc(&CreateChain)},
                                   std::pair{"vkDestroySwapchainKHR", Proc(&DestroyChain)},
                                   std::pair{"vkGetSwapchainImagesKHR", Proc(&Images)},
                                   std::pair{"vkAcquireNextImageKHR", Proc(&Acquire)},
                                   std::pair{"vkQueuePresentKHR", Proc(&Present)},
                                   std::pair{"vkReleaseSwapchainImagesEXT", Proc(&Release)},
                                   std::pair{"vkCreateImageView", Proc(&CreateView)},
                                   std::pair{"vkDestroyImageView", Proc(&DestroyView)},
                                   std::pair{"vkCreateRenderPass", Proc(&CreatePass)},
                                   std::pair{"vkDestroyRenderPass", Proc(&DestroyPass)},
                                   std::pair{"vkCreateFramebuffer", Proc(&CreateFramebuffer)},
                                   std::pair{"vkDestroyFramebuffer", Proc(&DestroyFramebuffer)},
                                   std::pair{"vkCreateFence", Proc(&CreateFence)},
                                   std::pair{"vkDestroyFence", Proc(&DestroyFence)},
                                   std::pair{"vkWaitForFences", Proc(&WaitFences)},
                                   std::pair{"vkResetFences", Proc(&ResetFences)},
                                   std::pair{"vkCreateSemaphore", Proc(&CreateSemaphore)},
                                   std::pair{"vkDestroySemaphore", Proc(&DestroySemaphore)},
                                   std::pair{"vkCreateCommandPool", Proc(&CreatePool)},
                                   std::pair{"vkDestroyCommandPool", Proc(&DestroyPool)},
                                   std::pair{"vkAllocateCommandBuffers", Proc(&Commands)},
                                   std::pair{"vkResetCommandBuffer", Proc(&ResetCommand)},
                                   std::pair{"vkBeginCommandBuffer", Proc(&BeginCommand)},
                                   std::pair{"vkEndCommandBuffer", Proc(&EndCommand)},
                                   std::pair{"vkCmdBeginRenderPass", Proc(&BeginPass)},
                                   std::pair{"vkCmdEndRenderPass", Proc(&EndPass)},
                                   std::pair{"vkQueueSubmit", Proc(&Submit)}};
            const auto found = std::ranges::find_if(table, [name](const auto &row) {
                return std::strcmp(row.first, name) == 0;
            });
            return found == table.end() ? nullptr : found->second;
        }

        /** @brief Uses the same explicit fake inventory for device-local procedure resolution. */
        static PFN_vkVoidFunction VKAPI_CALL ResolveDevice(VkDevice, const char *name) {
            return Resolve({}, name);
        }
    };
}  // namespace Horo::Render::AndroidVulkan::Testing
