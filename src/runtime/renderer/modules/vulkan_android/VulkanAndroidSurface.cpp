#include "VulkanAndroidRuntime.h"

#include <algorithm>
#include <format>
#include <limits>

namespace Horo::Render::AndroidVulkan {
    namespace {
        /** @brief Selects only an actual legacy SDR storage/space pair, without assuming a calibrated output. */
        Result<VkFormat> SelectFormat(const Dispatch &vk, VkPhysicalDevice physical, VkSurfaceKHR surface) {
            std::array<VkSurfaceFormatKHR, 256> formats{};
            std::uint32_t count{};
            auto result = vk.surfaceFormats(physical, surface, &count, nullptr);
            if (result != VK_SUCCESS)
                return Result<VkFormat>::Failure(NativeError("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
            if (count > formats.size())
                return Result<VkFormat>::Failure(MakeError(Errors::Capacity));
            result = vk.surfaceFormats(physical, surface, &count, formats.data());
            if (result != VK_SUCCESS)
                return Result<VkFormat>::Failure(NativeError("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
            for (const auto &format : std::span{formats}.first(count))
                if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                    (format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM))
                    return Result<VkFormat>::Success(format.format);
            return Result<VkFormat>::Failure(
                MakeError(Errors::Unsupported, "The Android surface exposes no supported RGB8 UNORM/sRGB nonlinear pair."));
        }

        /** @brief Confirms actual FIFO presentation support from the same physical-device/surface pair. */
        Result<void> AdmitMode(const Dispatch &vk, VkPhysicalDevice physical, VkSurfaceKHR surface) {
            std::array<VkPresentModeKHR, 32> modes{};
            std::uint32_t count{};
            auto result = vk.surfaceModes(physical, surface, &count, nullptr);
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
            if (count > modes.size())
                return Result<void>::Failure(MakeError(Errors::Capacity));
            result = vk.surfaceModes(physical, surface, &count, modes.data());
            if (result != VK_SUCCESS)
                return Result<void>::Failure(NativeError("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
            if (std::ranges::find(std::span{modes}.first(count), VK_PRESENT_MODE_FIFO_KHR) == std::span{modes}.first(count).end())
                return Result<void>::Failure(MakeError(Errors::Unsupported, "The Android surface does not expose FIFO presentation."));
            return Result<void>::Success();
        }

        /** @brief Selects only reported opaque or explicitly native-window-default alpha composition. */
        Result<VkCompositeAlphaFlagBitsKHR> SelectCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
            if ((supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0)
                return Result<VkCompositeAlphaFlagBitsKHR>::Success(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
            if ((supported & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0)
                return Result<VkCompositeAlphaFlagBitsKHR>::Success(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
            return Result<VkCompositeAlphaFlagBitsKHR>::Failure(
                MakeError(Errors::Unsupported,
                          std::format("Android surface alpha mask {} exposes neither OPAQUE nor native-window-default INHERIT.",
                                      supported)));
        }

        /** @brief Checks the closed attachment/single-layer profile against actual native surface limits. */
        bool AdmitsSurface(const VkSurfaceCapabilitiesKHR &caps, FramebufferExtent extent) {
            const std::array facts{extent.IsValid(),
                                   bool(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                                   caps.maxImageArrayLayers >= 1,
                                   extent.width >= caps.minImageExtent.width,
                                   extent.height >= caps.minImageExtent.height,
                                   extent.width <= caps.maxImageExtent.width,
                                   extent.height <= caps.maxImageExtent.height};
            return std::ranges::all_of(facts, [](bool fact) {
                return fact;
            });
        }

        /** @brief Bounds actual swapchain image requirements without wrapping or inventing native limits. */
        bool AdmitsImageCount(const VkSurfaceCapabilitiesKHR &caps) {
            return caps.minImageCount > 0 && caps.minImageCount <= 8 &&
                   (caps.maxImageCount == 0 || caps.minImageCount <= caps.maxImageCount);
        }
    }  // namespace

    /** @brief Imports one current retained occurrence and validates presentation on the already selected graphics queue. */
    Result<void> Runtime::CreateSurface(Platform::Android::AndroidWindowLease lease) {
        if (surface_ || !lease.IsCurrent())
            return Result<void>::Failure(MakeError(Errors::Invalid));
        surface_.emplace();
        surface_->window = std::move(lease);
        const VkAndroidSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                 .window = static_cast<ANativeWindow *>(surface_->window.NativeHandle())};
        auto result = vk_.createSurface(instance_, &info, nullptr, &surface_->native);
        if (result != VK_SUCCESS) {
            DestroySurface();
            return Result<void>::Failure(NativeError("vkCreateAndroidSurfaceKHR", result));
        }
        VkBool32 supported{};
        result = vk_.surfaceSupport(physical_, family_, surface_->native, &supported);
        if (result != VK_SUCCESS) {
            DestroySurface();
            return Result<void>::Failure(NativeError("vkGetPhysicalDeviceSurfaceSupportKHR", result));
        }
        if (!supported) {
            DestroySurface();
            return Result<void>::Failure(
                MakeError(Errors::Unsupported, "The initialized graphics queue cannot present to this exact Android window."));
        }
        return FinishSurface(CreateSwapchain());
    }

    /** @brief Completes the exact frozen realization and rolls back candidate children while preserving primary native failures. */
    Result<void> Runtime::FinishSurface(const Result<void> &prepared) {
        const auto outcome = prepared.HasValue() ? RenderSurfaceRealization::Ready : RenderSurfaceRealization::Lost;
        const auto completed = CompleteRealization(outcome);
        if (prepared.HasError()) {
            DestroySurface();
            return prepared;
        }
        if (completed.HasError()) {
            DestroySurface();
            return completed;
        }
        return Result<void>::Success();
    }

    /** @brief Freezes exact queried window/output facts before creating native swapchain children. */
    Result<void> Runtime::BeginRealization() {
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Errors::Capacity));
        const auto snapshot = lifecycle_->Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        const auto kind =
            snapshot.Value().state == RenderSurfaceState::Unattached ? RenderSurfaceCommandKind::Attach : RenderSurfaceCommandKind::Recover;
        const RenderSurfaceConfiguration target{surface_->extent, {}, surface_->window.Snapshot().revision};
        const auto queued = lifecycle_->Queue({++sequence_, kind, target});
        if (queued.HasError())
            return Result<void>::Failure(queued.ErrorValue());
        auto frozen = lifecycle_->BeginFrameBoundary();
        if (frozen.HasError())
            return Result<void>::Failure(frozen.ErrorValue());
        candidate_ = std::move(frozen).Value();
        return Result<void>::Success();
    }

    /** @brief Publishes only the matching native candidate outcome, never a reconstructed or relabeled transition. */
    Result<void> Runtime::CompleteRealization(RenderSurfaceRealization outcome) {
        if (!candidate_)
            return Result<void>::Success();
        const auto completed = lifecycle_->Complete(*candidate_, outcome);
        if (completed.HasError())
            return Result<void>::Failure(completed.ErrorValue());
        candidate_.reset();
        return Result<void>::Success();
    }

    /** @brief Resolves exact native surface facts before allocating any swapchain children. */
    Result<VkSwapchainCreateInfoKHR> Runtime::ResolveSwapchain() {
        const auto format = SelectFormat(vk_, physical_, surface_->native);
        if (format.HasError())
            return Result<VkSwapchainCreateInfoKHR>::Failure(format.ErrorValue());
        const auto mode = AdmitMode(vk_, physical_, surface_->native);
        if (mode.HasError())
            return Result<VkSwapchainCreateInfoKHR>::Failure(mode.ErrorValue());
        VkSurfaceCapabilitiesKHR caps{};
        const auto result = vk_.surfaceCapabilities(physical_, surface_->native, &caps);
        if (result != VK_SUCCESS)
            return Result<VkSwapchainCreateInfoKHR>::Failure(NativeError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
        const auto alpha = SelectCompositeAlpha(caps.supportedCompositeAlpha);
        if (alpha.HasError())
            return Result<VkSwapchainCreateInfoKHR>::Failure(alpha.ErrorValue());
        const auto window = surface_->window.Snapshot();
        surface_->extent = {window.extent.width, window.extent.height};
        surface_->format = format.Value();
        surface_->compositeAlpha = alpha.Value();
        if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
            surface_->extent = {caps.currentExtent.width, caps.currentExtent.height};
        if (!AdmitsSurface(caps, surface_->extent))
            return Result<VkSwapchainCreateInfoKHR>::Failure(
                MakeError(Errors::Unsupported,
                          std::format("Android surface rejects the attachment/single-layer profile: usage={}, alpha={}, layers={}, "
                                      "extent={}x{}, "
                                      "limits={}x{}..{}x{}.",
                                      caps.supportedUsageFlags, caps.supportedCompositeAlpha, caps.maxImageArrayLayers,
                                      surface_->extent.width, surface_->extent.height, caps.minImageExtent.width,
                                      caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height)));
        if (!AdmitsImageCount(caps))
            return Result<VkSwapchainCreateInfoKHR>::Failure(MakeError(Errors::Capacity));
        const VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                            .surface = surface_->native,
                                            .minImageCount = caps.minImageCount,
                                            .imageFormat = surface_->format,
                                            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                            .imageExtent = {surface_->extent.width, surface_->extent.height},
                                            .imageArrayLayers = 1,
                                            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                            .preTransform = caps.currentTransform,
                                            .compositeAlpha = surface_->compositeAlpha,
                                            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                            .clipped = VK_TRUE};
        return Result<VkSwapchainCreateInfoKHR>::Success(info);
    }

    /** @brief Creates one admitted FIFO chain with bounded images; no second queue or unbudgeted scene resources exist. */
    Result<void> Runtime::CreateSwapchain() {
        const auto resolved = ResolveSwapchain();
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        const auto frozen = BeginRealization();
        if (frozen.HasError())
            return frozen;
        const auto result = vk_.createSwapchain(device_, &resolved.Value(), nullptr, &surface_->swapchain);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateSwapchainKHR", result));
        auto pass = CreateRenderPass();
        if (pass.HasError())
            return pass;
        return CreateImages();
    }

    /** @brief Materializes only the complete bounded native image inventory, retaining partial children for rollback. */
    Result<void> Runtime::CreateImages() {
        auto result = vk_.swapchainImages(device_, surface_->swapchain, &surface_->imageCount, nullptr);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkGetSwapchainImagesKHR", result));
        if (surface_->imageCount == 0 || surface_->imageCount > 8)
            return Result<void>::Failure(MakeError(Errors::Capacity));
        std::array<VkImage, 8> images{};
        result = vk_.swapchainImages(device_, surface_->swapchain, &surface_->imageCount, images.data());
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkGetSwapchainImagesKHR", result));
        for (std::uint32_t index = 0; index < surface_->imageCount; ++index) {
            surface_->images[index].image = images[index];
            auto image = CreateImage(index);
            if (image.HasError())
                return image;
        }
        return Result<void>::Success();
    }

    /** @brief Uses a Vulkan 1.1 render pass to clear the acquired image and publish PRESENT layout. */
    Result<void> Runtime::CreateRenderPass() {
        const VkAttachmentDescription attachment{.format = surface_->format,
                                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                 .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                           .colorAttachmentCount = 1,
                                           .pColorAttachments = &color};
        const VkSubpassDependency dependency{.srcSubpass = VK_SUBPASS_EXTERNAL,
                                             .dstSubpass = 0,
                                             .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                          .attachmentCount = 1,
                                          .pAttachments = &attachment,
                                          .subpassCount = 1,
                                          .pSubpasses = &subpass,
                                          .dependencyCount = 1,
                                          .pDependencies = &dependency};
        const auto result = vk_.createRenderPass(device_, &info, nullptr, &surface_->renderPass);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateRenderPass", result));
        return Result<void>::Success();
    }

    /** @brief Owns each image's view/framebuffer and independently observable native presentation fence. */
    Result<void> Runtime::CreateImage(std::uint32_t index) {
        auto &image = surface_->images[index];
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = image.image,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = surface_->format,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        auto result = vk_.createView(device_, &view, nullptr, &image.view);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateImageView", result));
        const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                  .renderPass = surface_->renderPass,
                                                  .attachmentCount = 1,
                                                  .pAttachments = &image.view,
                                                  .width = surface_->extent.width,
                                                  .height = surface_->extent.height,
                                                  .layers = 1};
        result = vk_.createFramebuffer(device_, &framebuffer, nullptr, &image.framebuffer);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateFramebuffer", result));
        const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vk_.createSemaphore(device_, &semaphore, nullptr, &image.finished);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateSemaphore", result));
        const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vk_.createFence(device_, &fence, nullptr, &image.presentFence);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateFence(present)", result));
        return Result<void>::Success();
    }

    /** @brief Publishes exact native success/loss through the canonical generation authority. */
    Result<void> Runtime::PublishSurface(RenderSurfaceCommandKind kind, RenderSurfaceRealization outcome) {
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Errors::Capacity));
        RenderSurfaceCommand command{.sequence = ++sequence_, .kind = kind};
        if (outcome == RenderSurfaceRealization::Ready)
            command.target = RenderSurfaceConfiguration{surface_->extent, {}, surface_->window.Snapshot().revision};
        const auto queued = lifecycle_->Queue(command);
        if (queued.HasError())
            return Result<void>::Failure(queued.ErrorValue());
        const auto frozen = lifecycle_->BeginFrameBoundary();
        if (frozen.HasError())
            return Result<void>::Failure(frozen.ErrorValue());
        const auto complete = lifecycle_->Complete(frozen.Value(), outcome);
        if (complete.HasError())
            return Result<void>::Failure(complete.ErrorValue());
        return Result<void>::Success();
    }

    /** @brief Destroys only this generation's already retired children before releasing its authentic native-window lease. */
    void Runtime::DestroySurface() noexcept {
        if (!surface_)
            return;
        for (auto &image : surface_->images) {
            if (image.framebuffer)
                vk_.destroyFramebuffer(device_, image.framebuffer, nullptr);
            if (image.view)
                vk_.destroyView(device_, image.view, nullptr);
            if (image.finished)
                vk_.destroySemaphore(device_, image.finished, nullptr);
            if (image.presentFence)
                vk_.destroyFence(device_, image.presentFence, nullptr);
        }
        if (surface_->renderPass)
            vk_.destroyRenderPass(device_, surface_->renderPass, nullptr);
        if (surface_->swapchain)
            vk_.destroySwapchain(device_, surface_->swapchain, nullptr);
        if (surface_->native)
            vk_.destroySurface(instance_, surface_->native, nullptr);
        surface_.reset();
    }
}  // namespace Horo::Render::AndroidVulkan
