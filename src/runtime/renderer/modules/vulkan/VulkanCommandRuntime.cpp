#include "VulkanCommandRuntimeInternal.h"

#include <bit>
#include <exception>
#include <new>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Loads one exact core device command without a legacy extension fallback. */
        template <typename Function>
        [[nodiscard]] Function Resolve(const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver, const VkDevice device,
                                       const char *name) {
            return std::bit_cast<Function>(resolver(device, name));
        }

        /** @brief Rejects duplicate logical/native queues before creating command resources. */
        [[nodiscard]] bool ValidQueues(const std::span<const VulkanCommandQueue> queues) {
            if (queues.empty() || queues.size() > RenderGraphSubmissionLimits::MaximumQueues)
                return false;
            for (std::size_t index = 0; index < queues.size(); ++index) {
                const auto &queue = queues[index];
                if (!queue.identity.IsValid() || queue.family == VK_QUEUE_FAMILY_IGNORED || queue.native == VK_NULL_HANDLE)
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (queues[prior].identity == queue.identity || queues[prior].native == queue.native)
                        return false;
            }
            return true;
        }
    }  // namespace

    Result<void> VulkanCommandRuntime::Impl::LoadDispatch(const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        dispatch.createPool = Resolve<PFN_vkCreateCommandPool>(resolver, device, "vkCreateCommandPool");
        dispatch.destroyPool = Resolve<PFN_vkDestroyCommandPool>(resolver, device, "vkDestroyCommandPool");
        dispatch.allocateBuffers = Resolve<PFN_vkAllocateCommandBuffers>(resolver, device, "vkAllocateCommandBuffers");
        dispatch.resetBuffer = Resolve<PFN_vkResetCommandBuffer>(resolver, device, "vkResetCommandBuffer");
        dispatch.beginBuffer = Resolve<PFN_vkBeginCommandBuffer>(resolver, device, "vkBeginCommandBuffer");
        dispatch.endBuffer = Resolve<PFN_vkEndCommandBuffer>(resolver, device, "vkEndCommandBuffer");
        dispatch.createSemaphore = Resolve<PFN_vkCreateSemaphore>(resolver, device, "vkCreateSemaphore");
        dispatch.destroySemaphore = Resolve<PFN_vkDestroySemaphore>(resolver, device, "vkDestroySemaphore");
        dispatch.submit = Resolve<PFN_vkQueueSubmit2>(resolver, device, "vkQueueSubmit2");
        dispatch.counter = Resolve<PFN_vkGetSemaphoreCounterValue>(resolver, device, "vkGetSemaphoreCounterValue");
        dispatch.wait = Resolve<PFN_vkWaitSemaphores>(resolver, device, "vkWaitSemaphores");
        if (dispatch.createPool == nullptr || dispatch.destroyPool == nullptr || dispatch.allocateBuffers == nullptr ||
            dispatch.resetBuffer == nullptr || dispatch.beginBuffer == nullptr || dispatch.endBuffer == nullptr ||
            dispatch.createSemaphore == nullptr || dispatch.destroySemaphore == nullptr || dispatch.submit == nullptr ||
            dispatch.counter == nullptr || dispatch.wait == nullptr)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EntryPointMissing,
                                                   "Vulkan core command, synchronization2 or timeline dispatch is incomplete."));
        return Result<void>::Success();
    }

    Result<void> VulkanCommandRuntime::Impl::CreateQueue(Queue &queue, const std::uint32_t slots) {
        const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                           .queueFamilyIndex = queue.binding.family};
        if (const auto result = dispatch.createPool(device, &pool, nullptr, &queue.pool); result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateCommandPool", result));
        const VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                             .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                             .initialValue = 0};
        const VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        if (const auto result = dispatch.createSemaphore(device, &semaphore, nullptr, &queue.timeline); result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkCreateSemaphore", result));
        std::vector<VkCommandBuffer> buffers(slots);
        const VkCommandBufferAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                     .commandPool = queue.pool,
                                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                     .commandBufferCount = slots};
        if (const auto result = dispatch.allocateBuffers(device, &allocation, buffers.data()); result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkAllocateCommandBuffers", result));
        queue.slots.resize(slots);
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            if (buffers[index] == VK_NULL_HANDLE)
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::NativeCallFailed, "Vulkan returned a null allocated command buffer."));
            queue.slots[index].command = buffers[index];
        }
        return Result<void>::Success();
    }

    VulkanCommandRuntime::VulkanCommandRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanCommandRuntime::~VulkanCommandRuntime() {
        if (impl_->device != VK_NULL_HANDLE) {
            const auto begun = BeginShutdown();
            const auto drained = PollDrain();
            if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                std::terminate();
        }
    }

    /** @copydoc VulkanCommandRuntime::Initialize */
    Result<void> VulkanCommandRuntime::Initialize(const VkDevice device, const std::span<const VulkanCommandQueue> queues,
                                                  const std::uint32_t slotsPerQueue, const std::uint64_t maximumTimelineDifference,
                                                  const std::uint64_t deviceIncarnation,
                                                  const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        if (impl_->owner != std::thread::id{})
            return Result<void>::Failure(MakeError(VulkanBackendErrors::AlreadyInitialized));
        if (device == VK_NULL_HANDLE || !resolver || !ValidQueues(queues) || slotsPerQueue == 0 || maximumTimelineDifference == 0 ||
            deviceIncarnation == 0 || slotsPerQueue > RenderGraphSubmissionLimits::MaximumSubmissions)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        impl_->device = device;
        impl_->maximumTimelineDifference = maximumTimelineDifference;
        impl_->deviceIncarnation = deviceIncarnation;
        impl_->owner = std::this_thread::get_id();
        if (const auto loaded = impl_->LoadDispatch(resolver); loaded.HasError()) {
            impl_->DestroyPrepared();
            return loaded;
        }
        try {
            impl_->queues.resize(queues.size());
            for (std::size_t index = 0; index < queues.size(); ++index) {
                auto &queue = impl_->queues[index];
                queue.binding = queues[index];
                if (const auto created = impl_->CreateQueue(queue, slotsPerQueue); created.HasError()) {
                    impl_->DestroyPrepared();
                    return created;
                }
            }
        } catch (const std::bad_alloc &) {
            impl_->DestroyPrepared();
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::ResourceCreationFailed, "Preparing bounded Vulkan command storage failed."));
        }
        impl_->closed = false;
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Begin */
    Result<VulkanCommandTicket> VulkanCommandRuntime::Begin(const RenderQueueId identity) {
        if (const auto access = impl_->Access(true); access.HasError())
            return Result<VulkanCommandTicket>::Failure(access.ErrorValue());
        auto *queue = impl_->FindQueue(identity);
        if (queue == nullptr)
            return Result<VulkanCommandTicket>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        if (const auto polled = Poll(); polled.HasError())
            return Result<VulkanCommandTicket>::Failure(polled.ErrorValue());
        if (impl_->nextTicket == std::numeric_limits<std::uint64_t>::max())
            return Result<VulkanCommandTicket>::Failure(MakeError(VulkanBackendErrors::TimelineExhausted));
        for (auto &slot : queue->slots) {
            if (slot.state != Impl::State::Idle)
                continue;
            if (const auto result = impl_->dispatch.resetBuffer(slot.command, 0); result != VK_SUCCESS)
                return Result<VulkanCommandTicket>::Failure(impl_->NativeError("vkResetCommandBuffer", result));
            const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            if (const auto result = impl_->dispatch.beginBuffer(slot.command, &begin); result != VK_SUCCESS)
                return Result<VulkanCommandTicket>::Failure(impl_->NativeError("vkBeginCommandBuffer", result));
            slot.ticket = VulkanCommandTicket{impl_->nextTicket++};
            slot.state = Impl::State::Recording;
            return Result<VulkanCommandTicket>::Success(slot.ticket);
        }
        return Result<VulkanCommandTicket>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
    }

    /** @copydoc VulkanCommandRuntime::RecordingBuffer */
    Result<VkCommandBuffer> VulkanCommandRuntime::RecordingBuffer(const VulkanCommandTicket ticket) const {
        if (const auto access = impl_->Access(); access.HasError())
            return Result<VkCommandBuffer>::Failure(access.ErrorValue());
        const auto [queue, slot] = impl_->FindTicket(ticket);
        static_cast<void>(queue);
        if (slot == nullptr || slot->state != Impl::State::Recording)
            return Result<VkCommandBuffer>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<VkCommandBuffer>::Success(slot->command);
    }

    /** @copydoc VulkanCommandRuntime::Finish */
    Result<void> VulkanCommandRuntime::Finish(const VulkanCommandTicket ticket) {
        if (const auto access = impl_->Access(true); access.HasError())
            return access;
        const auto [queue, slot] = impl_->FindTicket(ticket);
        static_cast<void>(queue);
        if (slot == nullptr || slot->state != Impl::State::Recording)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (const auto result = impl_->dispatch.endBuffer(slot->command); result != VK_SUCCESS)
            return Result<void>::Failure(impl_->NativeError("vkEndCommandBuffer", result));
        slot->state = Impl::State::Executable;
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Retain */
    Result<void> VulkanCommandRuntime::Retain(const VulkanCommandTicket ticket, VulkanCommandRetention &retention) {
        if (const auto access = impl_->Access(true); access.HasError())
            return access;
        const auto [queue, slot] = impl_->FindTicket(ticket);
        static_cast<void>(queue);
        if (slot == nullptr || slot->state == Impl::State::Pending || slot->retention != nullptr)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        retention.Retain();
        slot->retention = &retention;
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Abort */
    void VulkanCommandRuntime::Abort(const VulkanCommandTicket ticket) noexcept {
        if (impl_->owner != std::this_thread::get_id())
            return;
        const auto [queue, slot] = impl_->FindTicket(ticket);
        static_cast<void>(queue);
        if (slot != nullptr && slot->state != Impl::State::Pending && slot->state != Impl::State::Lost) {
            slot->ReleaseRetention();
            slot->state = Impl::State::Idle;
        }
    }

    /** @copydoc VulkanCommandRuntime::QueueFamily */
    Result<std::uint32_t> VulkanCommandRuntime::QueueFamily(const RenderQueueId identity) const {
        if (const auto access = impl_->Access(); access.HasError())
            return Result<std::uint32_t>::Failure(access.ErrorValue());
        if (const auto *queue = impl_->FindQueue(identity))
            return Result<std::uint32_t>::Success(queue->binding.family);
        return Result<std::uint32_t>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
    }

    /** @copydoc VulkanCommandRuntime::IsActive */
    bool VulkanCommandRuntime::IsActive() const noexcept {
        return impl_->owner != std::thread::id{} && !impl_->closed && !impl_->lost;
    }
}  // namespace Horo::Render::Detail
