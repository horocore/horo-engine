#pragma once

#include "VulkanCommandRuntime.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>

namespace Horo::Render::Detail {
    struct VulkanCommandRuntime::Impl {
        enum class State : std::uint8_t {
            Idle,
            Recording,
            Executable,
            Pending,
            Lost
        };

        struct Slot {
            VkCommandBuffer command{VK_NULL_HANDLE};
            VulkanCommandTicket ticket;
            RenderTimelinePoint completion;
            VulkanCommandRetention *retention{nullptr};
            State state{State::Idle};

            void ReleaseRetention() noexcept {
                if (retention != nullptr)
                    retention->Release();
                retention = nullptr;
            }
        };

        struct Queue {
            VulkanCommandQueue binding;
            VkCommandPool pool{VK_NULL_HANDLE};
            VkSemaphore timeline{VK_NULL_HANDLE};
            std::uint64_t signaled{};
            std::uint64_t observed{};
            std::vector<Slot> slots;

            struct IssuedRange {
                std::uint64_t first{}, last{};
            };

            std::array<IssuedRange, 512> issued{};
            std::size_t issuedCount{};

            [[nodiscard]] bool WasIssued(const std::uint64_t value) const noexcept {
                return std::ranges::any_of(std::span{issued}.first(issuedCount), [value](const IssuedRange &range) {
                    return value >= range.first && value <= range.last;
                });
            }

            [[nodiscard]] bool CanIssue(const std::uint64_t value) const noexcept {
                return (signaled != std::numeric_limits<std::uint64_t>::max() && value == signaled + 1 && issuedCount != 0) ||
                       issuedCount < issued.size();
            }

            void RecordIssued(const std::uint64_t value) noexcept {
                if (issuedCount != 0 && value == signaled + 1)
                    issued[issuedCount - 1].last = value;
                else
                    issued[issuedCount++] = {value, value};
            }
        };

        struct Dispatch {
            PFN_vkCreateCommandPool createPool{};
            PFN_vkDestroyCommandPool destroyPool{};
            PFN_vkAllocateCommandBuffers allocateBuffers{};
            PFN_vkResetCommandBuffer resetBuffer{};
            PFN_vkBeginCommandBuffer beginBuffer{};
            PFN_vkEndCommandBuffer endBuffer{};
            PFN_vkCreateSemaphore createSemaphore{};
            PFN_vkDestroySemaphore destroySemaphore{};
            PFN_vkQueueSubmit2 submit{};
            PFN_vkGetSemaphoreCounterValue counter{};
            PFN_vkWaitSemaphores wait{};
        } dispatch;

        VkDevice device{VK_NULL_HANDLE};
        std::thread::id owner;
        std::vector<Queue> queues;
        std::uint64_t nextTicket{1};
        std::uint64_t lastOrder{};
        std::uint64_t maximumTimelineDifference{};
        std::uint64_t deviceIncarnation{};
        bool closed{true};
        bool lost{false};

        [[nodiscard]] Result<void> Access(const bool requireAdmission = false) const {
            if (owner == std::thread::id{})
                return Result<void>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
            if (std::this_thread::get_id() != owner)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
            if (lost)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::DeviceLost));
            if (requireAdmission && closed)
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::InvalidCommandState, "Command admission is closed for teardown."));
            return Result<void>::Success();
        }

        [[nodiscard]] Error NativeError(const char *operation, const VkResult result) {
            if (result == VK_ERROR_DEVICE_LOST) {
                lost = true;
                closed = true;
            }
            return MakeError(result == VK_ERROR_DEVICE_LOST ? VulkanBackendErrors::DeviceLost : VulkanBackendErrors::NativeCallFailed,
                             std::string{operation} + " returned VkResult " + std::to_string(result) + '.');
        }

        [[nodiscard]] Queue *FindQueue(const RenderQueueId identity) noexcept {
            const auto found = std::ranges::find(queues, identity, [](const Queue &queue) {
                return queue.binding.identity;
            });
            return found == queues.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const Queue *FindQueue(const RenderQueueId identity) const noexcept {
            const auto found = std::ranges::find(queues, identity, [](const Queue &queue) {
                return queue.binding.identity;
            });
            return found == queues.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] std::pair<Queue *, Slot *> FindTicket(const VulkanCommandTicket ticket) noexcept {
            if (ticket.value != 0)
                for (auto &queue : queues)
                    for (auto &slot : queue.slots)
                        if (slot.ticket == ticket && slot.state != State::Idle)
                            return {&queue, &slot};
            return {};
        }

        [[nodiscard]] Result<void> LoadDispatch(const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        [[nodiscard]] Result<void> CreateQueue(Queue &queue, std::uint32_t slots);
        [[nodiscard]] Result<void> ValidateSubmission(const Queue &queue, const RenderQueueSubmission &submission) const;

        [[nodiscard]] Result<void> ValidateWaits(std::span<const RenderTimelinePoint> waits,
                                                 std::span<const RenderGraphQueueSubmission> earlier) const;

        [[nodiscard]] bool HasPending() const noexcept {
            return std::ranges::any_of(queues, [](const Queue &queue) {
                return std::ranges::any_of(queue.slots, [](const Slot &slot) {
                    return slot.state == State::Pending;
                });
            });
        }

        void DestroyPrepared() noexcept {
            for (auto &queue : queues) {
                for (auto &slot : queue.slots)
                    slot.ReleaseRetention();
                queue.slots.clear();
                if (queue.pool != VK_NULL_HANDLE)
                    dispatch.destroyPool(device, queue.pool, nullptr);
                if (queue.timeline != VK_NULL_HANDLE)
                    dispatch.destroySemaphore(device, queue.timeline, nullptr);
            }
            queues.clear();
            device = VK_NULL_HANDLE;
            owner = {};
            closed = true;
            lost = false;
            lastOrder = 0;
        }
    };
}  // namespace Horo::Render::Detail
