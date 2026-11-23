#include "VulkanCommandRuntimeInternal.h"

#include <cmath>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Admits timestamp facts for queues that can encode the measured pipeline stages. */
        [[nodiscard]] bool ValidTimestampProperties(const VulkanCommandQueue &queue) noexcept {
            return (queue.supportsGraphics || queue.supportsCompute) && queue.timestampValidBits > 0 && queue.timestampValidBits <= 64 &&
                   std::isfinite(queue.timestampPeriodNs) && queue.timestampPeriodNs > 0.0F;
        }

        /** @brief Subtracts in the queue's counter width and rejects zero or unrepresentable durations. */
        [[nodiscard]] std::chrono::nanoseconds Duration(const VulkanCommandQueue &queue, std::uint64_t begin, std::uint64_t end) {
            const auto bitMask = std::numeric_limits<std::uint64_t>::max() >> (64U - queue.timestampValidBits);
            const auto ticks = (end - begin) & bitMask;
            const double nanoseconds = static_cast<double>(ticks) * queue.timestampPeriodNs;
            if (nanoseconds < 1.0 || nanoseconds >= std::ldexp(1.0, std::numeric_limits<std::chrono::nanoseconds::rep>::digits))
                return {};
            return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(nanoseconds)};
        }
    }  // namespace

    /** @brief Reserves one disjoint timestamp pair per command slot and bounded completed history. */
    Result<void> VulkanCommandRuntime::Impl::PrepareTimestamps(Queue &queue, const std::uint32_t slots) {
        if (!ValidTimestampProperties(queue.binding) || !timestamps.IsComplete())
            return Result<void>::Success();
        const VkQueryPoolCreateInfo request{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                            .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = 2 * slots};
        VkQueryPool pool{};
        const auto result = timestamps.create(device, &request, nullptr, &pool);
        if (result == VK_ERROR_DEVICE_LOST)
            return Result<void>::Failure(NativeError("vkCreateQueryPool", result));
        if (result == VK_SUCCESS && pool != VK_NULL_HANDLE) {
            queue.timestampPool = pool;
            queue.timings.resize(slots);
        }
        return Result<void>::Success();
    }

    /** @brief Reads only retired query pairs. A failed or unavailable optional sample leaves no timing evidence. */
    Result<void> VulkanCommandRuntime::Impl::ReadTimestamps(Queue &queue, const Slot &slot) {
        if (!slot.timed)
            return Result<void>::Success();
        std::array<std::uint64_t, 4> data{};
        constexpr auto stride = 2 * sizeof(std::uint64_t);
        const auto result = timestamps.results(device, queue.timestampPool, queue.FirstTimestamp(slot), 2, sizeof(data), data.data(),
                                               stride, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_ERROR_DEVICE_LOST)
            return Result<void>::Failure(NativeError("vkGetQueryPoolResults", result));
        if (result != VK_SUCCESS || data[1] == 0 || data[3] == 0)
            return Result<void>::Success();
        if (const auto duration = Duration(queue.binding, data[0], data[2]); duration.count() > 0) {
            queue.timings[queue.nextTiming] = {slot.completion.value, duration};
            queue.nextTiming = (queue.nextTiming + 1) % queue.timings.size();
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanCommandRuntime::SupportsGpuDuration */
    Result<bool> VulkanCommandRuntime::SupportsGpuDuration(const RenderQueueId identity) const {
        if (const auto access = impl_->Access(); access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        const auto *queue = impl_->FindQueue(identity);
        if (!queue)
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        return Result<bool>::Success(queue->timestampPool != VK_NULL_HANDLE);
    }

    /** @copydoc VulkanCommandRuntime::GpuDuration */
    Result<std::optional<std::chrono::nanoseconds>> VulkanCommandRuntime::GpuDuration(const RenderTimelinePoint completion) const {
        using Outcome = Result<std::optional<std::chrono::nanoseconds>>;
        if (const auto access = impl_->Access(); access.HasError())
            return Outcome::Failure(access.ErrorValue());
        const auto *queue = impl_->FindQueue(completion.queue);
        if (!queue || !completion.IsValid() || !queue->WasIssued(completion.value))
            return Outcome::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
        const auto found = std::ranges::find(queue->timings, completion.value, &Impl::Timing::signal);
        if (found == queue->timings.end())
            return Outcome::Success(std::nullopt);
        return Outcome::Success(found->duration);
    }
}  // namespace Horo::Render::Detail
