#include "VulkanAndroidRuntime.h"

#include <algorithm>

namespace Horo::Render::AndroidVulkan {
    /** @brief Rejects every unsupported render operation before recording or submitting commands. */
    Result<void> Runtime::ValidatePass(const RenderPassDescriptor &pass) const {
        if (!pass.id.IsValid() || pass.kind != RenderPassKind::Graphics || pass.staticMesh || !pass.primaryOutput)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        const auto &attachment = *pass.primaryOutput;
        const std::array supported{attachment.IsValid(), !attachment.outputColor,
                                   attachment.loadOperation == AttachmentLoadOperation::Clear,
                                   attachment.storeOperation == AttachmentStoreOperation::Store};
        if (!std::ranges::all_of(supported, [](bool fact) {
            return fact;
        }))
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        if (surface_->compositeAlpha == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR && attachment.clearColor.alpha != 1.0F)
            return Result<void>::Failure(
                MakeError(Errors::Unsupported,
                          "Native-window-default INHERIT composition requires clear alpha exactly one; native opacity is not verified."));
        return Result<void>::Success();
    }

    /** @brief Submits one actual recorded Vulkan 1.1 command and its independently observed completion fence. */
    Result<void> Runtime::Submit(Slot &slot) {
        auto result = vk_.resetFences(device_, 1, &slot.commandFence);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkResetFences(command)", result));
        auto &image = surface_->images[slot.image];
        const VkSubmitInfo info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &slot.command,
                                .signalSemaphoreCount = 1,
                                .pSignalSemaphores = &image.finished};
        result = vk_.submit(queue_, 1, &info, slot.commandFence);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkQueueSubmit", result));
        slot.submitted = true;
        return Result<void>::Success();
    }

    /** @brief Tracks actual present-fence debt independently of rendering completion and preserves rejected-image retirement. */
    Result<void> Runtime::QueuePresent(Slot &slot) {
        auto &image = surface_->images[slot.image];
        auto result = vk_.resetFences(device_, 1, &image.presentFence);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkResetFences(present)", result));
        const VkSwapchainPresentFenceInfoEXT fence{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                                                   .swapchainCount = 1,
                                                   .pFences = &image.presentFence};
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .pNext = &fence,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &image.finished,
                                    .swapchainCount = 1,
                                    .pSwapchains = &surface_->swapchain,
                                    .pImageIndices = &slot.image};
        result = vk_.present(queue_, &info);
        const std::array queuedResults{VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR};
        if (std::ranges::find(queuedResults, result) != queuedResults.end()) {
            image.presentPending = true;
            slot.acquired = false;
        }
        if (result != VK_SUCCESS)
            surface_->retiring = true;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR)
            return Result<void>::Failure(WrapError(Errors::Pending, NativeError("vkQueuePresentKHR", result)));
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Result<void>::Failure(NativeError("vkQueuePresentKHR", result));
        return Result<void>::Success();
    }

    /** @brief Publishes success only after actual native submit and present admission for the exact still-current frame. */
    Result<void> Runtime::Present(FrameToken frame) {
        const auto access = Access();
        if (access.HasError())
            return access;
        auto *slot = Active(frame);
        if (!slot || !encoded_)
            return Result<void>::Failure(MakeError(Errors::Invalid));
        if (!surface_->window.IsCurrent())
            return Result<void>::Failure(MakeError(Errors::Pending));
        const auto submitted = Submit(*slot);
        if (submitted.HasError())
            return submitted;
        const auto presented = QueuePresent(*slot);
        if (presented.HasError()) {
            slot->returning = slot->acquired;
            surface_->retiring = true;
        }
        slot->frame = {};
        active_ = {};
        encoded_ = false;
        return presented;
    }
}  // namespace Horo::Render::AndroidVulkan
