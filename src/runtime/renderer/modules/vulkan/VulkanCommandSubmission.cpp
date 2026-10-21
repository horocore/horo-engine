#include "VulkanCommandRuntimeInternal.h"

namespace Horo::Render::Detail {
    Result<void> VulkanCommandRuntime::Impl::ValidateSubmission(const Queue &queue, const RenderQueueSubmission &submission) const {
        if (!submission.IsValid() || submission.queue != queue.binding.identity || submission.order.value <= lastOrder ||
            submission.signal.value <= queue.signaled || !queue.CanIssue(submission.signal.value) ||
            submission.waits.size() > RenderGraphSubmissionLimits::MaximumQueues)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        if (submission.signal.value - queue.observed > maximumTimelineDifference)
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::TimelineInvalid, "Signal exceeds the admitted device timeline value-difference limit."));
        return ValidateWaits(submission.waits, {});
    }

    /** @brief Checks exact accepted or earlier-batch producer signals and rejects duplicate queue waits. */
    Result<void> VulkanCommandRuntime::Impl::ValidateWaits(const std::span<const RenderTimelinePoint> waits,
                                                           const std::span<const RenderGraphQueueSubmission> earlier) const {
        for (std::size_t index = 0; index < waits.size(); ++index) {
            const auto wait = waits[index];
            const auto *producer = FindQueue(wait.queue);
            const bool earlierSignal = std::ranges::any_of(earlier, [&wait](const RenderGraphQueueSubmission &record) {
                return record.submission.signal == wait;
            });
            if (producer == nullptr || (!earlierSignal && !producer->WasIssued(wait.value)))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::TimelineInvalid, "A GPU wait has no exact issued producer point."));
            for (std::size_t previous = 0; previous < index; ++previous)
                if (waits[previous].queue == wait.queue)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid, "Duplicate queue timeline wait."));
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::ValidateBatch */
    Result<void> VulkanCommandRuntime::ValidateBatch(const std::span<const RenderGraphQueueSubmission> records) const {
        if (auto access = impl_->Access(true); access.HasError())
            return access;
        if (records.empty() || records.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        std::array<std::uint64_t, RenderGraphSubmissionLimits::MaximumQueues> signaled{};
        std::array<std::size_t, RenderGraphSubmissionLimits::MaximumQueues> available{};
        std::array<std::size_t, RenderGraphSubmissionLimits::MaximumQueues> ranges{};
        for (std::size_t index = 0; index < impl_->queues.size(); ++index) {
            signaled[index] = impl_->queues[index].signaled;
            ranges[index] = impl_->queues[index].issuedCount;
            available[index] =
                static_cast<std::size_t>(std::ranges::count(impl_->queues[index].slots, Impl::State::Idle, &Impl::Slot::state));
        }
        const auto queueIndex = [&](const RenderQueueId id) {
            const auto found = std::ranges::find(impl_->queues, id, [](const Impl::Queue &queue) {
                return queue.binding.identity;
            });
            return static_cast<std::size_t>(std::distance(impl_->queues.begin(), found));
        };
        auto order = impl_->lastOrder;
        for (std::size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
            const auto &record = records[recordIndex];
            const auto &value = record.submission;
            const auto index = queueIndex(value.queue);
            if (!value.IsValid() || index == impl_->queues.size() || value.waits.size() > RenderGraphSubmissionLimits::MaximumQueues ||
                value.order.value <= order || value.signal.value <= signaled[index] ||
                value.signal.value - impl_->queues[index].observed > impl_->maximumTimelineDifference)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid,
                                                       "Submission batch differs from admitted timeline history or device bounds."));
            if ((ranges[index] == 0 || value.signal.value != signaled[index] + 1) && ++ranges[index] > impl_->queues[index].issued.size())
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::TimelineInvalid, "Bounded issued-point history is exhausted by discontinuous signals."));
            if (available[index] == 0)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
            if (auto waits = impl_->ValidateWaits(value.waits, records.first(recordIndex)); waits.HasError())
                return waits;
            --available[index];
            signaled[index] = value.signal.value;
            order = value.order.value;
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Submit */
    Result<void> VulkanCommandRuntime::Submit(const VulkanCommandTicket ticket, const RenderQueueSubmission &submission) {
        if (const auto access = impl_->Access(true); access.HasError())
            return access;
        const auto [queue, slot] = impl_->FindTicket(ticket);
        if (queue == nullptr || slot->state != Impl::State::Executable)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (const auto valid = impl_->ValidateSubmission(*queue, submission); valid.HasError())
            return valid;
        std::array<VkSemaphoreSubmitInfo, RenderGraphSubmissionLimits::MaximumQueues> waits{};
        for (std::size_t index = 0; index < submission.waits.size(); ++index) {
            const auto point = submission.waits[index];
            waits[index] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                            .semaphore = impl_->FindQueue(point.queue)->timeline,
                            .value = point.value,
                            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                           .semaphore = queue->timeline,
                                           .value = submission.signal.value,
                                           .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = slot->command};
        const VkSubmitInfo2 native{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .waitSemaphoreInfoCount = static_cast<std::uint32_t>(submission.waits.size()),
                                   .pWaitSemaphoreInfos = waits.data(),
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &command,
                                   .signalSemaphoreInfoCount = 1,
                                   .pSignalSemaphoreInfos = &signal};
        if (const auto result = impl_->dispatch.submit(queue->binding.native, 1, &native, VK_NULL_HANDLE); result != VK_SUCCESS) {
            auto error = impl_->NativeError("vkQueueSubmit2", result);
            if (impl_->lost)
                slot->state = Impl::State::Lost;
            return Result<void>::Failure(std::move(error));
        }
        slot->completion = submission.signal;
        slot->state = Impl::State::Pending;
        queue->RecordIssued(submission.signal.value);
        queue->signaled = submission.signal.value;
        impl_->lastOrder = submission.order.value;
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Poll */
    Result<void> VulkanCommandRuntime::Poll() {
        if (const auto access = impl_->Access(); access.HasError())
            return access;
        for (auto &queue : impl_->queues) {
            std::uint64_t observed = 0;
            if (const auto result = impl_->dispatch.counter(impl_->device, queue.timeline, &observed); result != VK_SUCCESS)
                return Result<void>::Failure(impl_->NativeError("vkGetSemaphoreCounterValue", result));
            if (observed < queue.observed || observed > queue.signaled)
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::TimelineInvalid, "The native completion counter escaped its accepted timeline domain."));
            queue.observed = observed;
            for (auto &slot : queue.slots) {
                if (slot.state == Impl::State::Pending && slot.completion.value <= observed) {
                    slot.ReleaseRetention();
                    slot.state = Impl::State::Idle;
                }
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::Observe */
    Result<bool> VulkanCommandRuntime::Observe(const RenderCpuWait &wait) {
        if (const auto access = impl_->Access(); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        const auto *queue = impl_->FindQueue(wait.completion.queue);
        if (wait.expectedDeviceIncarnation == 0 || wait.expectedDeviceIncarnation != impl_->deviceIncarnation)
            return Result<bool>::Failure(
                MakeError(VulkanBackendErrors::TimelineInvalid, "GPU completion belongs to another or unspecified device incarnation."));
        if (!wait.IsValid() || wait.timeout > std::chrono::seconds{30} || queue == nullptr || !queue->WasIssued(wait.completion.value))
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        if (wait.purpose == RenderCpuWaitPurpose::Poll) {
            if (const auto polled = Poll(); polled.HasError())
                return Result<bool>::Failure(polled.ErrorValue());
            return Result<bool>::Success(queue->observed >= wait.completion.value);
        }
        const VkSemaphoreWaitInfo native{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                         .semaphoreCount = 1,
                                         .pSemaphores = &queue->timeline,
                                         .pValues = &wait.completion.value};
        const auto result = impl_->dispatch.wait(impl_->device, &native, static_cast<std::uint64_t>(wait.timeout.count()));
        if (result == VK_TIMEOUT)
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::CompletionTimeout));
        if (result != VK_SUCCESS)
            return Result<bool>::Failure(impl_->NativeError("vkWaitSemaphores", result));
        if (const auto polled = Poll(); polled.HasError())
            return Result<bool>::Failure(polled.ErrorValue());
        return Result<bool>::Success(queue->observed >= wait.completion.value);
    }

    /** @copydoc VulkanCommandRuntime::BeginShutdown */
    Result<void> VulkanCommandRuntime::BeginShutdown() {
        if (impl_->owner == std::thread::id{})
            return Result<void>::Success();
        if (impl_->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
        impl_->closed = true;
        for (auto &queue : impl_->queues)
            for (auto &slot : queue.slots)
                if (slot.state == Impl::State::Recording || slot.state == Impl::State::Executable)
                    Abort(slot.ticket);
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::PollDrain */
    Result<RenderBackendDrainProgress> VulkanCommandRuntime::PollDrain() {
        using DrainResult = Result<RenderBackendDrainProgress>;
        if (impl_->owner == std::thread::id{})
            return DrainResult::Success(RenderBackendDrainProgress::Drained);
        if (impl_->owner != std::this_thread::get_id())
            return DrainResult::Failure(MakeError(VulkanBackendErrors::WrongThread));
        if (!impl_->closed)
            return DrainResult::Failure(
                MakeError(VulkanBackendErrors::InvalidCommandState, "BeginShutdown must close admission before drain polling."));
        if (!impl_->lost)
            if (const auto polled = Poll(); polled.HasError())
                return DrainResult::Failure(polled.ErrorValue());
        if (!impl_->lost && impl_->HasPending())
            return DrainResult::Success(RenderBackendDrainProgress::Pending);
        impl_->DestroyPrepared();
        return DrainResult::Success(RenderBackendDrainProgress::Drained);
    }
}  // namespace Horo::Render::Detail
