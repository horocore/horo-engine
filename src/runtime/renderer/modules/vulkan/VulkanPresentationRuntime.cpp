#include "Horo/Runtime/Render/RenderPresentationOutput.h"
#include "VulkanPresentationRuntimeState.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <limits>
#include <optional>
#include <thread>

namespace Horo::Render::Detail {
    using VulkanPresentationDetail::NativeFailure;
    using VulkanPresentationDetail::Pending;

    /** @brief Resolves required same-instance WSI and same-device object operations before acquisition. */
    Result<void> VulkanPresentationRuntime::Impl::LoadDispatch(const VulkanPresentationInitialization &request) {
        const auto instanceFunction = [&]<typename T>(const char *name) {
            return std::bit_cast<T>(request.instanceResolver(request.instance, name));
        };
        const auto deviceFunction = [&]<typename T>(const char *name) {
            return std::bit_cast<T>(request.deviceResolver(request.device, name));
        };
        surfaceQueries.capabilities =
            instanceFunction.operator()<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        surfaceQueries.formats =
            instanceFunction.operator()<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
        surfaceQueries.modes =
            instanceFunction.operator()<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
        swapchainOperations.createChain = deviceFunction.operator()<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
        swapchainOperations.destroyChain = deviceFunction.operator()<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
        swapchainOperations.chainImages = deviceFunction.operator()<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
        swapchainOperations.acquire = deviceFunction.operator()<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
        swapchainOperations.present = deviceFunction.operator()<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
        swapchainOperations.releaseImages = deviceFunction.operator()<PFN_vkReleaseSwapchainImagesEXT>("vkReleaseSwapchainImagesEXT");
        deviceObjects.createView = deviceFunction.operator()<PFN_vkCreateImageView>("vkCreateImageView");
        deviceObjects.destroyView = deviceFunction.operator()<PFN_vkDestroyImageView>("vkDestroyImageView");
        deviceObjects.createSemaphore = deviceFunction.operator()<PFN_vkCreateSemaphore>("vkCreateSemaphore");
        deviceObjects.destroySemaphore = deviceFunction.operator()<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        deviceObjects.createFence = deviceFunction.operator()<PFN_vkCreateFence>("vkCreateFence");
        deviceObjects.destroyFence = deviceFunction.operator()<PFN_vkDestroyFence>("vkDestroyFence");
        deviceObjects.fenceStatus = deviceFunction.operator()<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        deviceObjects.resetFences = deviceFunction.operator()<PFN_vkResetFences>("vkResetFences");
        if (!surfaceQueries.capabilities || !surfaceQueries.formats || !surfaceQueries.modes || !swapchainOperations.createChain ||
            !swapchainOperations.destroyChain || !swapchainOperations.chainImages || !swapchainOperations.acquire ||
            !swapchainOperations.present || !swapchainOperations.releaseImages || !deviceObjects.createView || !deviceObjects.destroyView ||
            !deviceObjects.createSemaphore || !deviceObjects.destroySemaphore || !deviceObjects.createFence ||
            !deviceObjects.destroyFence || !deviceObjects.fenceStatus || !deviceObjects.resetFences)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EntryPointMissing));
        return Result<void>::Success();
    }

    /** @brief Selects legacy encoded SDR storage without adding a second hardware transfer encoding. */
    Result<VkSurfaceFormatKHR> VulkanPresentationRuntime::Impl::SelectFormat() const {
        std::array<VkSurfaceFormatKHR, 256> supported{};
        std::uint32_t count{};
        if (const auto result = surfaceQueries.formats(context.physical, context.surface, &count, nullptr); result != VK_SUCCESS)
            return Result<VkSurfaceFormatKHR>::Failure(NativeFailure("vkGetPhysicalDeviceSurfaceFormatsKHR(count)", result));
        if (count == 0 || count > supported.size())
            return Result<VkSurfaceFormatKHR>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
        if (const auto result = surfaceQueries.formats(context.physical, context.surface, &count, supported.data()); result != VK_SUCCESS)
            return Result<VkSurfaceFormatKHR>::Failure(NativeFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result));
        // The selected editor pipeline remains bound to this device-lifetime format.
        // A resize may not reinterpret it under another attachment encoding/channel layout.
        if (count == 1 && supported[0].format == VK_FORMAT_UNDEFINED && supported[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return Result<VkSurfaceFormatKHR>::Success(
                {format != VK_FORMAT_UNDEFINED ? format : VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
        const std::array preferred{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        for (const auto requested : preferred) {
            if (format != VK_FORMAT_UNDEFINED && requested != format)
                continue;
            for (const auto &candidate : std::span{supported}.first(count))
                if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && candidate.format == requested)
                    return Result<VkSurfaceFormatKHR>::Success(candidate);
        }
        return Result<VkSurfaceFormatKHR>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
    }

    /** @brief Admits the exact requested present mode; no fallback is selected. */
    Result<VkPresentModeKHR> VulkanPresentationRuntime::Impl::SelectMode(const PresentMode mode) const {
        std::array<VkPresentModeKHR, 32> supported{};
        std::uint32_t count{};
        if (const auto result = surfaceQueries.modes(context.physical, context.surface, &count, nullptr); result != VK_SUCCESS)
            return Result<VkPresentModeKHR>::Failure(NativeFailure("vkGetPhysicalDeviceSurfacePresentModesKHR(count)", result));
        if (count == 0 || count > supported.size())
            return Result<VkPresentModeKHR>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
        if (const auto result = surfaceQueries.modes(context.physical, context.surface, &count, supported.data()); result != VK_SUCCESS)
            return Result<VkPresentModeKHR>::Failure(NativeFailure("vkGetPhysicalDeviceSurfacePresentModesKHR", result));
        const auto requested = mode == PresentMode::Fifo ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (std::ranges::find(std::span{supported}.first(count), requested) == std::span{supported}.first(count).end())
            return Result<VkPresentModeKHR>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
        return Result<VkPresentModeKHR>::Success(requested);
    }

    /** @brief Allocates only the bounded swapchain image wrappers after a successful explicit realization. */
    Result<void> VulkanPresentationRuntime::Impl::CreateImages() {
        if (const auto result = swapchainOperations.chainImages(context.device, chain, &imageCount, nullptr); result != VK_SUCCESS)
            return Result<void>::Failure(NativeFailure("vkGetSwapchainImagesKHR(count)", result));
        if (imageCount == 0 || imageCount > MaximumImages) {
            imageCount = 0;
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EnumerationLimitExceeded));
        }
        std::array<VkImage, MaximumImages> native{};
        if (const auto result = swapchainOperations.chainImages(context.device, chain, &imageCount, native.data()); result != VK_SUCCESS)
            return Result<void>::Failure(NativeFailure("vkGetSwapchainImagesKHR", result));
        const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (std::uint32_t index = 0; index < imageCount; ++index) {
            auto &image = images[index];
            image.image = native[index];
            const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                             .image = image.image,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                             .format = format,
                                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (const auto result = deviceObjects.createView(context.device, &view, nullptr, &image.view); result != VK_SUCCESS)
                return Result<void>::Failure(NativeFailure("vkCreateImageView(present)", result));
            if (const auto result = deviceObjects.createSemaphore(context.device, &semaphore, nullptr, &image.finished);
                result != VK_SUCCESS)
                return Result<void>::Failure(NativeFailure("vkCreateSemaphore(present)", result));
            if (const auto result = deviceObjects.createFence(context.device, &fence, nullptr, &image.presentFence); result != VK_SUCCESS)
                return Result<void>::Failure(NativeFailure("vkCreateFence(present)", result));
        }
        return Result<void>::Success();
    }

    /** @brief Resolves an exact immutable surface request before any native chain allocation. */
    Result<VkSwapchainCreateInfoKHR> VulkanPresentationRuntime::Impl::SelectChainInfo(const FramebufferExtent requested,
                                                                                      const PresentMode mode) const {
        VkSurfaceCapabilitiesKHR support{};
        if (const auto result = surfaceQueries.capabilities(context.physical, context.surface, &support); result != VK_SUCCESS)
            return Result<VkSwapchainCreateInfoKHR>::Failure(NativeFailure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
        const auto selectedFormat = SelectFormat();
        if (selectedFormat.HasError())
            return Result<VkSwapchainCreateInfoKHR>::Failure(selectedFormat.ErrorValue());
        const auto selectedMode = SelectMode(mode);
        if (selectedMode.HasError())
            return Result<VkSwapchainCreateInfoKHR>::Failure(selectedMode.ErrorValue());
        if ((support.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
            (support.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0)
            return Result<VkSwapchainCreateInfoKHR>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
        const VkExtent2D nativeExtent{requested.width, requested.height};
        if ((support.currentExtent.width != std::numeric_limits<std::uint32_t>::max() &&
             (support.currentExtent.width != requested.width || support.currentExtent.height != requested.height)) ||
            requested.width < support.minImageExtent.width || requested.width > support.maxImageExtent.width ||
            requested.height < support.minImageExtent.height || requested.height > support.maxImageExtent.height)
            return Result<VkSwapchainCreateInfoKHR>::Failure(Pending());
        auto count = std::max(support.minImageCount, frameCount + 1);
        if (support.maxImageCount != 0)
            count = std::min(count, support.maxImageCount);
        if (count < support.minImageCount || count > MaximumImages)
            return Result<VkSwapchainCreateInfoKHR>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
        const bool separate = context.families[0] != context.families[1];
        const VkSwapchainCreateInfoKHR create{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                              .surface = context.surface,
                                              .minImageCount = count,
                                              .imageFormat = selectedFormat.Value().format,
                                              .imageColorSpace = selectedFormat.Value().colorSpace,
                                              .imageExtent = nativeExtent,
                                              .imageArrayLayers = 1,
                                              .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                              .imageSharingMode = separate ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                                              .queueFamilyIndexCount = separate ? 2U : 0U,
                                              .pQueueFamilyIndices = separate ? context.families.data() : nullptr,
                                              .preTransform = support.currentTransform,
                                              .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                              .presentMode = selectedMode.Value(),
                                              .clipped = VK_TRUE};
        return Result<VkSwapchainCreateInfoKHR>::Success(create);
    }

    /** @brief Creates one bounded chain only after its predecessor's independent debts have retired. */
    Result<void> VulkanPresentationRuntime::Impl::CreateChain(const VkSwapchainCreateInfoKHR &create) {
        if (const auto result = swapchainOperations.createChain(context.device, &create, nullptr, &chain); result != VK_SUCCESS)
            return Result<void>::Failure(NativeFailure("vkCreateSwapchainKHR", result));
        format = create.imageFormat;
        extent = {create.imageExtent.width, create.imageExtent.height};
        if (auto wrapped = CreateImages(); wrapped.HasError()) {
            DestroyChain();
            return wrapped;
        }
        return Result<void>::Success();
    }

    VulkanPresentationRuntime::VulkanPresentationRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanPresentationRuntime::~VulkanPresentationRuntime() {
        if (impl_->context.device == VK_NULL_HANDLE)
            return;
        const auto closing = BeginShutdown();
        const auto drained = PollDrain();
        if (closing.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
            std::terminate();
    }

    /** @copydoc VulkanPresentationRuntime::Initialize */
    Result<void> VulkanPresentationRuntime::Initialize(const VulkanPresentationInitialization &request) {
        if (impl_->context.device || !request.instance || !request.physical || !request.device || !request.surface ||
            !request.presentQueue || !request.instanceResolver || !request.deviceResolver || !request.observeCompletion ||
            request.frames == 0 || request.frames > MaximumFrames)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        if (auto loaded = impl_->LoadDispatch(request); loaded.HasError())
            return loaded;
        impl_->context.owner = std::this_thread::get_id();
        impl_->context.device = request.device;
        impl_->context.instance = request.instance;
        impl_->context.physical = request.physical;
        impl_->context.surface = request.surface;
        impl_->context.presentQueue = request.presentQueue;
        impl_->context.families = {request.graphicsFamily, request.presentFamily};
        impl_->frameCount = request.frames;
        impl_->context.observe = &request.observeCompletion;
        const VkFenceCreateInfo create{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto &slot : std::span{impl_->slots}.first(impl_->frameCount))
            if (const auto result = impl_->deviceObjects.createFence(impl_->context.device, &create, nullptr, &slot.acquireFence);
                result != VK_SUCCESS)
                return Result<void>::Failure(NativeFailure("vkCreateFence(acquire)", result));
        return Result<void>::Success();
    }

    /** @copydoc VulkanPresentationRuntime::Resize */
    Result<void> VulkanPresentationRuntime::Resize(const FramebufferExtent extent, const PresentMode mode) {
        if (auto access = impl_->Access(true); access.HasError())
            return access;
        if (!extent.IsValid() || (mode != PresentMode::Fifo && mode != PresentMode::Immediate))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        if (std::ranges::any_of(std::span{impl_->slots}.first(impl_->frameCount), [](const auto &slot) {
            return slot.phase == Impl::Phase::Active || slot.phase == Impl::Phase::Submitted;
        }))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        // Reject incompatible output facts transactionally, before abandoning an acquired image
        // or retiring a still-valid predecessor. Reuse these exact facts after retirement.
        const auto selected = impl_->SelectChainInfo(extent, mode);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        if (!impl_->resizing) {
            impl_->resizing = true;
            impl_->requestedExtent = extent;
            impl_->requestedMode = mode;
            impl_->pendingSlot.reset();
            for (auto &slot : std::span{impl_->slots}.first(impl_->frameCount))
                if (slot.phase == Impl::Phase::Acquiring || slot.phase == Impl::Phase::Ready)
                    slot.phase = Impl::Phase::Abandoned;
        } else if (impl_->requestedExtent != extent || impl_->requestedMode != mode) {
            // The existing surface authority explicitly cancels/replaces its frozen candidate first.
            impl_->requestedExtent = extent;
            impl_->requestedMode = mode;
        }
        const auto drained = impl_->PollRetirement();
        if (drained.HasError())
            return Result<void>::Failure(drained.ErrorValue());
        if (!drained.Value())
            return Result<void>::Failure(Pending());
        impl_->DestroyChain();
        if (auto created = impl_->CreateChain(selected.Value()); created.HasError())
            return created;
        impl_->resizing = false;
        impl_->needsReconfiguration = false;
        return Result<void>::Success();
    }

    /** @copydoc VulkanPresentationRuntime::Impl::AcquireSlot */
    Result<bool> VulkanPresentationRuntime::Impl::AcquireSlot() {
        const auto availableSlots = std::span{slots}.first(frameCount);
        const auto free = std::ranges::find(availableSlots, Phase::Free, &Slot::phase);
        if (free == availableSlots.end())
            return Result<bool>::Success(false);
        const auto result = swapchainOperations.acquire(context.device, chain, 0, VK_NULL_HANDLE, free->acquireFence, &free->image);
        if (result == VK_NOT_READY || result == VK_TIMEOUT)
            return Result<bool>::Success(false);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR)
                needsReconfiguration = true;
            else
                faulted = true;
            return Result<bool>::Failure(NativeFailure("vkAcquireNextImageKHR", result));
        }
        free->acquisitionPending = true;
        free->phase = Phase::Acquiring;
        pendingSlot = static_cast<std::size_t>(std::distance(availableSlots.begin(), free));
        if (free->image >= imageCount) {
            faulted = true;
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        }
        // Complete this valid acquisition before requesting explicit recovery for SUBOPTIMAL.
        if (result == VK_SUBOPTIMAL_KHR)
            needsReconfiguration = true;
        return Result<bool>::Success(true);
    }

    /** @copydoc VulkanPresentationRuntime::PollAvailability */
    Result<bool> VulkanPresentationRuntime::PollAvailability(const FramebufferExtent extent) {
        if (auto access = impl_->Access(true); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        if (impl_->resizing || !impl_->chain || extent != impl_->extent)
            return Result<bool>::Success(false);
        if (impl_->needsReconfiguration && !impl_->pendingSlot)
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported,
                                                   "The current swapchain requires an explicit surface recovery transition."));
        if (auto polled = impl_->PollRetirement(); polled.HasError())
            return Result<bool>::Failure(polled.ErrorValue());
        if (!impl_->pendingSlot) {
            const auto acquired = impl_->AcquireSlot();
            if (acquired.HasError() || !acquired.Value())
                return acquired;
        }
        if (auto polled = impl_->PollSlots(); polled.HasError())
            return Result<bool>::Failure(polled.ErrorValue());
        const auto &slot = impl_->slots[*impl_->pendingSlot];
        if (slot.phase != Impl::Phase::Ready)
            return Result<bool>::Success(false);
        auto &image = impl_->images[slot.image];
        if (image.presentPending) {
            const auto completed = impl_->FenceReady(image.presentFence);
            if (completed.HasError())
                return Result<bool>::Failure(completed.ErrorValue());
            if (!completed.Value())
                return Result<bool>::Success(false);
            image.presentPending = false;
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc VulkanPresentationRuntime::OutputFormat */
    Result<VkFormat> VulkanPresentationRuntime::OutputFormat() const {
        if (const auto access = impl_->Access(true); access.HasError())
            return Result<VkFormat>::Failure(access.ErrorValue());
        if (!impl_->chain || impl_->format == VK_FORMAT_UNDEFINED)
            return Result<VkFormat>::Failure(MakeError(RenderPresentationErrors::DrawableUnavailable));
        return Result<VkFormat>::Success(impl_->format);
    }

    /** @copydoc VulkanPresentationRuntime::BeginFrame */
    Result<VulkanPresentationFrame> VulkanPresentationRuntime::BeginFrame(const FrameToken frame) {
        if (auto access = impl_->Access(true); access.HasError())
            return Result<VulkanPresentationFrame>::Failure(access.ErrorValue());
        if (!frame.IsValid() || !impl_->pendingSlot || impl_->slots[*impl_->pendingSlot].phase != Impl::Phase::Ready ||
            std::ranges::any_of(std::span{impl_->slots}.first(impl_->frameCount), [](const auto &slot) {
            return slot.phase == Impl::Phase::Active || slot.phase == Impl::Phase::Submitted;
        }))
            return Result<VulkanPresentationFrame>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        auto &slot = impl_->slots[*impl_->pendingSlot];
        auto &image = impl_->images[slot.image];
        slot.frame = frame;
        slot.phase = Impl::Phase::Active;
        impl_->pendingSlot.reset();
        return Result<VulkanPresentationFrame>::Success({image.image, image.view, impl_->format, image.finished, impl_->extent});
    }

    /** @copydoc VulkanPresentationRuntime::Submitted */
    Result<void> VulkanPresentationRuntime::Submitted(const FrameToken frame, const RenderTimelinePoint completion) {
        if (auto access = impl_->Access(true); access.HasError())
            return access;
        const auto slots = std::span{impl_->slots}.first(impl_->frameCount);
        const auto slot = std::ranges::find(slots, frame, &Impl::Slot::frame);
        if (!frame.IsValid() || !completion.IsValid() || slot == slots.end() || slot->phase != Impl::Phase::Active)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        slot->completion = completion;
        slot->phase = Impl::Phase::Submitted;
        return Result<void>::Success();
    }

    /** @copydoc VulkanPresentationRuntime::Present */
    Result<void> VulkanPresentationRuntime::Present(const FrameToken frame) {
        if (auto access = impl_->Access(true); access.HasError())
            return access;
        const auto slots = std::span{impl_->slots}.first(impl_->frameCount);
        const auto slot = std::ranges::find(slots, frame, &Impl::Slot::frame);
        if (!frame.IsValid() || slot == slots.end() || slot->phase != Impl::Phase::Submitted)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        auto &image = impl_->images[slot->image];
        if (image.presentPending)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (const auto result = impl_->deviceObjects.resetFences(impl_->context.device, 1, &image.presentFence); result != VK_SUCCESS)
            return Result<void>::Failure(NativeFailure("vkResetFences(present)", result));
        const VkSwapchainPresentFenceInfoEXT fence{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                                                   .swapchainCount = 1,
                                                   .pFences = &image.presentFence};
        const VkPresentInfoKHR request{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                       .pNext = &fence,
                                       .waitSemaphoreCount = 1,
                                       .pWaitSemaphores = &image.finished,
                                       .swapchainCount = 1,
                                       .pSwapchains = &impl_->chain,
                                       .pImageIndices = &slot->image};
        const auto result = impl_->swapchainOperations.present(impl_->context.presentQueue, &request);
        if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            return Result<void>::Failure(NativeFailure("vkQueuePresentKHR", result));
        if (result == VK_ERROR_DEVICE_LOST) {
            impl_->faulted = true;
            return Result<void>::Failure(NativeFailure("vkQueuePresentKHR", result));
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR &&
            result != VK_ERROR_SURFACE_LOST_KHR) {
            impl_->faulted = true;
            return Result<void>::Failure(NativeFailure("vkQueuePresentKHR(unqualified outcome)", result));
        }
        // Present's queue operations are enqueued even when the presentation engine rejects this image.
        image.presentPending = true;
        slot->phase = Impl::Phase::Presented;
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR)
            impl_->needsReconfiguration = true;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Result<void>::Failure(NativeFailure("vkQueuePresentKHR", result));
        return Result<void>::Success();
    }

    /** @copydoc VulkanPresentationRuntime::Abort */
    void VulkanPresentationRuntime::Abort(const FrameToken frame) noexcept {
        if (impl_->context.owner != std::this_thread::get_id())
            return;
        for (auto &slot : std::span{impl_->slots}.first(impl_->frameCount))
            if (frame.IsValid() && slot.frame == frame && (slot.phase == Impl::Phase::Active || slot.phase == Impl::Phase::Submitted)) {
                if (slot.phase == Impl::Phase::Submitted)
                    impl_->needsReconfiguration = true;
                slot.phase = Impl::Phase::Abandoned;
            }
    }

    /** @copydoc VulkanPresentationRuntime::BeginShutdown */
    Result<void> VulkanPresentationRuntime::BeginShutdown() {
        if (impl_->context.device == VK_NULL_HANDLE)
            return Result<void>::Success();
        if (auto access = impl_->Access(); access.HasError())
            return access;
        impl_->closing = true;
        impl_->pendingSlot.reset();
        for (auto &slot : std::span{impl_->slots}.first(impl_->frameCount))
            if (slot.phase != Impl::Phase::Free && slot.phase != Impl::Phase::Presented)
                slot.phase = Impl::Phase::Abandoned;
        return Result<void>::Success();
    }

    /** @copydoc VulkanPresentationRuntime::PollDrain */
    Result<RenderBackendDrainProgress> VulkanPresentationRuntime::PollDrain() {
        if (impl_->context.device == VK_NULL_HANDLE)
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
        if (auto access = impl_->Access(); access.HasError())
            return Result<RenderBackendDrainProgress>::Failure(access.ErrorValue());
        if (!impl_->closing)
            return Result<RenderBackendDrainProgress>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        const auto retired = impl_->PollRetirement();
        if (retired.HasError())
            return Result<RenderBackendDrainProgress>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Pending);
        impl_->DestroyChain();
        for (auto &slot : std::span{impl_->slots}.first(impl_->frameCount)) {
            if (slot.acquireFence)
                impl_->deviceObjects.destroyFence(impl_->context.device, slot.acquireFence, nullptr);
            slot = {};
        }
        impl_->context.device = VK_NULL_HANDLE;
        return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
    }
}  // namespace Horo::Render::Detail
