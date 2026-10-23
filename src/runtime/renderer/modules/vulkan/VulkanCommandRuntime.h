#pragma once

/** @file VulkanCommandRuntime.h
 * @brief Private bounded Vulkan command ownership and exact Horo timeline translation.
 */
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <functional>
#include <memory>
#include <vulkan/vulkan.h>

namespace Horo::Render::Detail {
    /** @brief Exact admitted logical queue mapped to one device-owned native queue. */
    struct VulkanCommandQueue {
        RenderQueueId identity;
        std::uint32_t family{VK_QUEUE_FAMILY_IGNORED};
        VkQueue native{VK_NULL_HANDLE};
        bool supportsGraphics{false};
    };

    /** @brief Non-reused owner-local command identity; no native handle crosses RenderApi. */
    struct VulkanCommandTicket {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const VulkanCommandTicket &) const noexcept = default;
    };

    /** @brief Preallocated owner-thread graph retention; final command completion releases exact frontend pins. */
    struct VulkanCommandRetention {
        IRenderGraphResourceLease *lease{nullptr};
        std::vector<std::shared_ptr<const void>> resources;
        std::size_t references{};

        void Retain() noexcept {
            ++references;
        }

        void Release() noexcept {
            if (--references == 0) {
                resources.clear();
                if (lease != nullptr)
                    lease->Release();
                lease = nullptr;
            }
        }
    };

    /**
     * @brief Device-owned pools, reusable command slots and exact timeline semaphores.
     * @details Initialization, encoding, submission, polling and shutdown are externally
     * serialized on the initializing render owner. No worker or completion callback
     * touches mutable state. All command/queue storage is prepared during Initialize;
     * ordinary Begin/Submit/Poll never allocate or CPU-wait. Borrowed device/queues
     * outlive this runtime. BeginShutdown/PollDrain retain pending native objects
     * and leases. The host owns its finite deadline and recovery policy; destruction
     * before Drained is a fail-closed invariant violation.
     */
    class VulkanCommandRuntime final {
    public:
        VulkanCommandRuntime();
        ~VulkanCommandRuntime();
        VulkanCommandRuntime(const VulkanCommandRuntime &) = delete;
        VulkanCommandRuntime &operator=(const VulkanCommandRuntime &) = delete;

        /** @brief Loads required core dispatch and transactionally prepares finite queue resources.
         * @param device Admitted Vulkan1.3 device with sync2/timeline features explicitly enabled.
         * @param queues Unique frontend queue identities and their exact native family/queue mappings.
         * @param slotsPerQueue Positive finite pending-command bound, at most128.
         * @param maximumTimelineDifference Exact selected-device timeline semaphore value-difference limit.
         * @param resolver Device-owned procedure resolver, borrowed only during this call.
         * @return Success or original typed initialization/native failure with rollback. */
        [[nodiscard]] Result<void> Initialize(VkDevice device, std::span<const VulkanCommandQueue> queues, std::uint32_t slotsPerQueue,
                                              std::uint64_t maximumTimelineDifference, std::uint64_t deviceIncarnation,
                                              const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        /** @brief Reserves one completed slot without waiting; returns typed busy when all are retained. */
        [[nodiscard]] Result<VulkanCommandTicket> Begin(RenderQueueId queue);
        /** @brief Returns a borrowed recording buffer for this exact owner-local ticket. */
        [[nodiscard]] Result<VkCommandBuffer> RecordingBuffer(VulkanCommandTicket ticket) const;
        /** @brief Ends native recording; failure leaves the unsent ticket abortable. */
        [[nodiscard]] Result<void> Finish(VulkanCommandTicket ticket);
        /** @brief Pins one prevalidated graph until every command sharing the retention completes or is abandoned unsent. */
        [[nodiscard]] Result<void> Retain(VulkanCommandTicket ticket, VulkanCommandRetention &retention);
        /** @brief Validates a complete exact batch against currently admitted timelines without mutation or allocation. */
        [[nodiscard]] Result<void> ValidateBatch(std::span<const RenderGraphQueueSubmission> records) const;
        /** @brief Submits an executable ticket through the canonical ordered timeline, optionally signaling its WSI tail.
         * @param ticket Exact executable command owner. @param submission Frontend-issued queue/order/waits/completion.
         * @param presentationSignal Borrowed binary semaphore owned by the acquired swapchain image; null for offscreen work.
         * @return Typed admission/native outcome; failure retains the unsent ticket and never publishes completion.
         * @details The WSI owner proves binary semaphore retirement independently of the canonical timeline. No wait occurs.
         */
        [[nodiscard]] Result<void> Submit(VulkanCommandTicket ticket, const RenderQueueSubmission &submission,
                                          VkSemaphore presentationSignal = VK_NULL_HANDLE);
        /** @brief Nonblocking completion drain; releases only proven-complete command and resident references. */
        [[nodiscard]] Result<void> Poll();
        /** @brief Observes exact admitted completion; only declared finite purposes may block. */
        [[nodiscard]] Result<bool> Observe(const RenderCpuWait &wait);
        /** @brief Discards only an unsent ticket; submitted work retains its completion ownership. */
        void Abort(VulkanCommandTicket ticket) noexcept;
        /** @brief Closes admission and aborts unsent tickets without waiting or abandoning submitted ownership. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Polls terminal command retirement without waiting; Pending retains all native parents and leases. */
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();
        /** @brief Returns the exact native family for one admitted logical queue. */
        [[nodiscard]] Result<std::uint32_t> QueueFamily(RenderQueueId queue) const;
        /** @brief Reports whether initialization committed and admission remains open. */
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
