#pragma once
/** @file VulkanAndroidRuntime.h @brief Sole private native Vulkan 1.1 device and generation ownership. */
#include "Horo/Platform/Internal/AndroidWindowHost.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycle.h"
#include "VulkanAndroidErrors.h"

#include <array>
#include <optional>
#include <span>
#include <thread>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

namespace Horo::Render::AndroidVulkan {
    /** @brief Exact native dispatch; production resolves it from the owned instance/device. */
    struct Dispatch final {
        PFN_vkGetInstanceProcAddr instanceProc{};
        PFN_vkGetDeviceProcAddr deviceProc{};
        PFN_vkDestroyInstance destroyInstance{};
        PFN_vkDestroyDevice destroyDevice{};
        PFN_vkDestroySurfaceKHR destroySurface{};
        PFN_vkCreateAndroidSurfaceKHR createSurface{};
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport{};
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities{};
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats{};
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR surfaceModes{};
        PFN_vkCreateSwapchainKHR createSwapchain{};
        PFN_vkDestroySwapchainKHR destroySwapchain{};
        PFN_vkGetSwapchainImagesKHR swapchainImages{};
        PFN_vkAcquireNextImageKHR acquire{};
        PFN_vkQueuePresentKHR present{};
        PFN_vkReleaseSwapchainImagesEXT releaseImages{};
        PFN_vkCreateImageView createView{};
        PFN_vkDestroyImageView destroyView{};
        PFN_vkCreateRenderPass createRenderPass{};
        PFN_vkDestroyRenderPass destroyRenderPass{};
        PFN_vkCreateFramebuffer createFramebuffer{};
        PFN_vkDestroyFramebuffer destroyFramebuffer{};
        PFN_vkCreateFence createFence{};
        PFN_vkDestroyFence destroyFence{};
        PFN_vkGetFenceStatus fenceStatus{};
        PFN_vkResetFences resetFences{};
        PFN_vkCreateSemaphore createSemaphore{};
        PFN_vkDestroySemaphore destroySemaphore{};
        PFN_vkCreateCommandPool createPool{};
        PFN_vkDestroyCommandPool destroyPool{};
        PFN_vkAllocateCommandBuffers allocateCommands{};
        PFN_vkResetCommandBuffer resetCommand{};
        PFN_vkBeginCommandBuffer beginCommand{};
        PFN_vkEndCommandBuffer endCommand{};
        PFN_vkCmdBeginRenderPass beginPass{};
        PFN_vkCmdEndRenderPass endPass{};
        PFN_vkQueueSubmit submit{};
    };

    /** @brief Native per-image ownership with independent submitted rendering and presentation proofs. */
    struct Image final {
        VkImage image{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        VkSemaphore finished{};
        VkFence presentFence{};
        bool presentPending{};
    };

    /** @brief Bounded command slot; acquisition and accepted command completion are distinct. */
    struct Slot final {
        VkFence acquireFence{}, commandFence{};
        VkCommandBuffer command{};
        std::uint32_t image{};
        bool acquiring{}, acquired{}, submitted{}, returning{};
        FrameToken frame;
    };

    /** @brief One retained authentic platform occurrence and all its device-owned presentation objects. */
    struct Surface final {
        Platform::Android::AndroidWindowLease window;
        VkSurfaceKHR native{};
        VkSwapchainKHR swapchain{};
        VkRenderPass renderPass{};
        VkFormat format{};
        FramebufferExtent extent;
        std::array<Image, 8> images{};
        std::uint32_t imageCount{};
        bool retiring{};
    };

    /** @brief Sole device/surface/frame owner; construction is inert and no operation waits for device idle. */
    class Runtime final {
    public:
        /** @brief Captures explicit owners and optional test-native resolver without acquiring resources. */
        explicit Runtime(Platform::Android::AndroidWindowHost &windows, PFN_vkGetInstanceProcAddr resolver = nullptr) noexcept;
        /** @brief Requires explicit nonblocking terminal drain before destroying active native owners. */
        ~Runtime();
        Runtime(const Runtime &) = delete;
        Runtime &operator=(const Runtime &) = delete;
        /** @brief Activates the actual admitted device before any native window or surface exists. */
        [[nodiscard]] Result<void> Initialize(const RenderBackendConfig &config);
        /** @brief Polls readiness and generation retirement without acquiring a swapchain image. */
        [[nodiscard]] Result<bool> Availability(FramebufferExtent extent);
        /** @brief Begins one exact available generation, or returns typed backpressure. */
        [[nodiscard]] Result<FrameToken> Begin(const FrameDescriptor &frame);
        /** @brief Validates and records the closed primary clear operation. */
        [[nodiscard]] Result<void> Execute(const RenderExecutionPlan &plan);
        /** @brief Submits and presents the exact active frame through native 1.1 operations. */
        [[nodiscard]] Result<void> Present(FrameToken frame);
        /** @brief Cancels an exact unsent frame without waiting or discarding native debt. */
        void Abort(FrameToken frame) noexcept;

        /** @brief Aborts only the actual current frame without fabricating a token. */
        void AbortActive() noexcept {
            Abort(active_);
        }

        /** @brief Closes new frame admission and requests native retirement. */
        [[nodiscard]] Result<void> Close();
        /** @brief Polls all debt and releases window leases before reporting terminal drain. */
        [[nodiscard]] Result<RenderBackendDrainProgress> Drain();
        /** @brief Checks exact owner lane and lifecycle access. */
        [[nodiscard]] Result<void> Access(bool allowClosed = false) const;
        RenderBackendCapabilities capabilities{.backend = RenderBackendId{"vulkan-android"}};

    private:
        [[nodiscard]] Result<void> OpenLoader();
        [[nodiscard]] Result<void> PrepareNative();
        [[nodiscard]] Result<void> CreateInstance();
        [[nodiscard]] Result<void> SelectDevice();
        [[nodiscard]] Result<void> CreateDevice(VkPhysicalDevice physical, std::uint32_t family);
        [[nodiscard]] Result<void> LoadDispatch();
        [[nodiscard]] Result<void> CreateSlots();
        [[nodiscard]] Result<void> CreateSurface(Platform::Android::AndroidWindowLease lease);
        [[nodiscard]] Result<void> CreateSwapchain();
        [[nodiscard]] Result<VkSwapchainCreateInfoKHR> ResolveSwapchain();
        [[nodiscard]] Result<void> CreateImages();
        [[nodiscard]] Result<void> BeginRealization();
        [[nodiscard]] Result<void> CompleteRealization(RenderSurfaceRealization outcome);
        [[nodiscard]] Result<void> FinishSurface(const Result<void> &prepared);
        [[nodiscard]] Result<void> CreateImage(std::uint32_t index);
        [[nodiscard]] Result<void> CreateRenderPass();
        [[nodiscard]] Result<bool> RetireSurface();
        [[nodiscard]] Result<bool> PollSlots();
        [[nodiscard]] Result<bool> AcquireImage();
        [[nodiscard]] Result<bool> PublishAcquisition(Slot &slot, VkResult result);
        void CompleteAcquisition(Slot &slot) noexcept;
        [[nodiscard]] Result<bool> PrepareAcquisition();
        [[nodiscard]] Result<bool> ObserveAcquired(Slot &slot);
        [[nodiscard]] Result<bool> EnsureSurface();
        [[nodiscard]] Result<bool> ImportCurrentWindow();
        [[nodiscard]] bool NeedsRetirement() const noexcept;
        [[nodiscard]] Result<bool> AcquireFailure(VkResult result);
        [[nodiscard]] Result<void> LoseSurface();
        [[nodiscard]] bool ReadySlot(const Slot &slot) const noexcept;
        [[nodiscard]] Result<void> ValidatePass(const RenderPassDescriptor &pass) const;
        [[nodiscard]] Result<void> Submit(Slot &slot);
        [[nodiscard]] Result<void> QueuePresent(Slot &slot);
        [[nodiscard]] Result<bool> PollSlot(Slot &slot);
        [[nodiscard]] Result<bool> Fence(VkFence fence) const;
        [[nodiscard]] Result<void> ReleaseImage(Slot &slot);
        [[nodiscard]] Result<void> PublishSurface(RenderSurfaceCommandKind kind, RenderSurfaceRealization outcome);
        [[nodiscard]] Result<void> Record(Slot &slot, const PrimaryOutputAttachment &attachment);
        void DestroySurface() noexcept;
        void DestroyDevice() noexcept;
        void Rollback() noexcept;
        [[nodiscard]] Slot *Active(FrameToken frame) noexcept;
        Platform::Android::AndroidWindowHost &windows_;
        Dispatch vk_;
        void *library_{};
        bool injected_{};
        VkInstance instance_{};
        VkPhysicalDevice physical_{};
        VkDevice device_{};
        VkQueue queue_{};
        std::uint32_t family_{};
        VkCommandPool pool_{};
        std::array<Slot, 8> slots_{};
        std::uint32_t slotCount_{};
        std::optional<Surface> surface_;
        std::optional<RenderSurfaceLifecycle> lifecycle_;
        std::optional<RenderSurfaceTransition> candidate_;
        RenderBackendConfig config_;
        std::thread::id owner_;
        FrameToken active_;
        std::uint64_t nextFrame_{1}, sequence_{};
        bool initialized_{}, closing_{}, encoded_{};
    };
}  // namespace Horo::Render::AndroidVulkan
