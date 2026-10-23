#pragma once

/** @file VulkanPresentationRuntimeState.h
 * @brief Private bounded image/acquisition state and retirement operations for the single WSI owner. */
#include "Horo/Runtime/Render/RenderPresentationOutput.h"
#include "VulkanPresentationRuntime.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <optional>
#include <thread>

namespace Horo::Render::Detail {
    namespace VulkanPresentationDetail {
        inline constexpr std::size_t MaximumImages = 16;
        inline constexpr std::size_t MaximumFrames = 8;

        /** @brief Preserves native failure and identifies terminal device loss. */
        inline Error NativeFailure(const char *operation, const VkResult result) {
            return MakeError(result == VK_ERROR_DEVICE_LOST ? VulkanBackendErrors::DeviceLost : VulkanBackendErrors::NativeCallFailed,
                             std::string{operation} + ": VkResult " + std::to_string(result));
        }

        /** @brief Temporary readiness is explicit and never a completed native realization. */
        inline Error Pending() {
            return MakeError(RenderPresentationErrors::DrawableUnavailable);
        }
    }  // namespace VulkanPresentationDetail

    using VulkanPresentationDetail::MaximumFrames;
    using VulkanPresentationDetail::MaximumImages;

    struct VulkanPresentationRuntime::Impl {
        enum class Phase {
            Free,
            Acquiring,
            Ready,
            Active,
            Submitted,
            Presented,
            Abandoned
        };

        struct Image {
            VkImage image{};
            VkImageView view{};
            VkSemaphore finished{};
            VkFence presentFence{};
            bool presentPending{};
        };

        struct Slot {
            VkFence acquireFence{};
            Phase phase{Phase::Free};
            bool acquisitionPending{};
            std::uint32_t image{};
            FrameToken frame;
            RenderTimelinePoint completion;
        };

        // Borrowed device/session facts remain subordinate to this one retirement owner.
        struct Context {
            VkInstance instance{};
            VkPhysicalDevice physical{};
            VkDevice device{};
            VkSurfaceKHR surface{};
            VkQueue presentQueue{};
            std::array<std::uint32_t, 2> families{};
            std::thread::id owner;
            const std::function<Result<bool>(RenderTimelinePoint)> *observe{};
        };

        struct SurfaceQueries {
            PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities{};
            PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats{};
            PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes{};
        };

        struct SwapchainOperations {
            PFN_vkCreateSwapchainKHR createChain{};
            PFN_vkDestroySwapchainKHR destroyChain{};
            PFN_vkGetSwapchainImagesKHR chainImages{};
            PFN_vkAcquireNextImageKHR acquire{};
            PFN_vkQueuePresentKHR present{};
            PFN_vkReleaseSwapchainImagesEXT releaseImages{};
        };

        struct DeviceObjects {
            PFN_vkCreateImageView createView{};
            PFN_vkDestroyImageView destroyView{};
            PFN_vkCreateSemaphore createSemaphore{};
            PFN_vkDestroySemaphore destroySemaphore{};
            PFN_vkCreateFence createFence{};
            PFN_vkDestroyFence destroyFence{};
            PFN_vkGetFenceStatus fenceStatus{};
            PFN_vkResetFences resetFences{};
        };

        Context context;
        SurfaceQueries surfaceQueries;
        SwapchainOperations swapchainOperations;
        DeviceObjects deviceObjects;

        std::uint32_t frameCount{};
        VkSwapchainKHR chain{};
        VkFormat format{};
        FramebufferExtent extent;
        std::array<Image, MaximumImages> images{};
        std::uint32_t imageCount{};
        std::array<Slot, MaximumFrames> slots{};
        std::optional<std::size_t> pendingSlot;
        bool closing{};
        bool resizing{};
        bool faulted{};
        bool needsReconfiguration{};
        FramebufferExtent requestedExtent;
        PresentMode requestedMode{PresentMode::Fifo};

        [[nodiscard]] Result<void> Access(bool admit = false) const {
            if (context.owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
            if (context.device == VK_NULL_HANDLE || (admit && (closing || faulted)))
                return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<bool> FenceReady(const VkFence fence) {
            const auto result = deviceObjects.fenceStatus(context.device, fence);
            if (result == VK_SUCCESS || result == VK_NOT_READY)
                return Result<bool>::Success(result == VK_SUCCESS);
            faulted = true;
            return Result<bool>::Failure(VulkanPresentationDetail::NativeFailure("vkGetFenceStatus", result));
        }

        [[nodiscard]] Result<void> ResetSlot(Slot &slot) {
            if (const auto result = deviceObjects.resetFences(context.device, 1, &slot.acquireFence); result != VK_SUCCESS)
                return Result<void>::Failure(VulkanPresentationDetail::NativeFailure("vkResetFences(acquire)", result));
            const auto fence = slot.acquireFence;
            slot = {.acquireFence = fence};
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ReturnImage(Slot &slot) {
            const VkReleaseSwapchainImagesInfoEXT request{.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                                                          .swapchain = chain,
                                                          .imageIndexCount = 1,
                                                          .pImageIndices = &slot.image};
            if (const auto result = swapchainOperations.releaseImages(context.device, &request); result != VK_SUCCESS)
                return Result<void>::Failure(VulkanPresentationDetail::NativeFailure("vkReleaseSwapchainImagesEXT", result));
            return ResetSlot(slot);
        }

        [[nodiscard]] Result<void> PollSlots() {
            for (auto &slot : std::span{slots}.first(frameCount)) {
                if (slot.phase == Phase::Free)
                    continue;
                if (slot.acquisitionPending) {
                    const auto ready = FenceReady(slot.acquireFence);
                    if (ready.HasError())
                        return Result<void>::Failure(ready.ErrorValue());
                    if (!ready.Value())
                        continue;
                    slot.acquisitionPending = false;
                    if (slot.phase == Phase::Acquiring)
                        slot.phase = Phase::Ready;
                }
                if (slot.phase != Phase::Abandoned && slot.phase != Phase::Presented)
                    continue;
                if (slot.completion.IsValid()) {
                    const auto ready = (*context.observe)(slot.completion);
                    if (ready.HasError())
                        return Result<void>::Failure(ready.ErrorValue());
                    if (!ready.Value())
                        continue;
                }
                if (auto retired = slot.phase == Phase::Abandoned ? ReturnImage(slot) : ResetSlot(slot); retired.HasError())
                    return retired;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<bool> PollRetirement() {
            if (auto slotsPolled = PollSlots(); slotsPolled.HasError())
                return Result<bool>::Failure(slotsPolled.ErrorValue());
            bool complete = std::ranges::all_of(std::span{slots}.first(frameCount), [](const Slot &slot) {
                return slot.phase == Phase::Free;
            });
            for (auto &image : std::span{images}.first(imageCount)) {
                if (!image.presentPending)
                    continue;
                const auto ready = FenceReady(image.presentFence);
                if (ready.HasError())
                    return Result<bool>::Failure(ready.ErrorValue());
                if (ready.Value())
                    image.presentPending = false;
                else
                    complete = false;
            }
            return Result<bool>::Success(complete);
        }

        void DestroyChain() noexcept {
            for (auto &image : std::span{images}.first(imageCount)) {
                if (image.view)
                    deviceObjects.destroyView(context.device, image.view, nullptr);
                if (image.finished)
                    deviceObjects.destroySemaphore(context.device, image.finished, nullptr);
                if (image.presentFence)
                    deviceObjects.destroyFence(context.device, image.presentFence, nullptr);
                image = {};
            }
            imageCount = 0;
            if (chain)
                swapchainOperations.destroyChain(context.device, chain, nullptr);
            chain = VK_NULL_HANDLE;
            extent = {};
        }

        [[nodiscard]] Result<void> CreateChain(const VkSwapchainCreateInfoKHR &create);
        [[nodiscard]] Result<VkSwapchainCreateInfoKHR> SelectChainInfo(FramebufferExtent requested, PresentMode mode) const;
        [[nodiscard]] Result<void> CreateImages();
        /** @brief Starts one exact fence acquisition while retaining every successful native obligation. */
        [[nodiscard]] Result<bool> AcquireSlot();
        [[nodiscard]] Result<VkSurfaceFormatKHR> SelectFormat() const;
        [[nodiscard]] Result<VkPresentModeKHR> SelectMode(PresentMode mode) const;
        [[nodiscard]] Result<void> LoadDispatch(const VulkanPresentationInitialization &request);
    };

}  // namespace Horo::Render::Detail
