#pragma once

#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanCommandRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <string_view>
#include <type_traits>

namespace Horo::Render::VulkanTest {
    template <typename Handle> [[nodiscard]] Handle NativeHandle(const std::uintptr_t value) noexcept {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    template <typename Function> [[nodiscard]] PFN_vkVoidFunction EraseFunction(const Function function) noexcept {
        return std::bit_cast<PFN_vkVoidFunction>(function);
    }

    struct Submission {
        VkQueue queue{};
        VkCommandBuffer command{};
        std::vector<VkSemaphoreSubmitInfo> waits;
        VkSemaphoreSubmitInfo signal{};
    };

    struct NativeState {
        std::uintptr_t next{100};
        int createdPools{};
        int destroyedPools{};
        int destroyedSemaphores{};
        int beginCalls{};
        int endCalls{};
        int waitCalls{};
        int counterCalls{};
        int failPoolAt{};
        VkResult submitResult{VK_SUCCESS};
        VkResult beginResult{VK_SUCCESS};
        VkResult endResult{VK_SUCCESS};
        VkResult counterResult{VK_SUCCESS};
        VkResult waitResult{VK_TIMEOUT};
        bool timelineOnly{true};
        std::map<VkSemaphore, std::uint64_t> counters;
        std::vector<Submission> accepted;
        std::vector<std::uint32_t> families;

        void Complete() {
            for (const auto &submission : accepted)
                counters[submission.signal.semaphore] = submission.signal.value;
        }
    };

    inline NativeState *native{};

    inline VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *,
                                                     VkCommandPool *pool) {
        ++native->createdPools;
        if (native->failPoolAt == native->createdPools)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        native->families.push_back(info->queueFamilyIndex);
        *pool = NativeHandle<VkCommandPool>(native->next++);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *) {
        ++native->destroyedPools;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *commands) {
        for (std::uint32_t index = 0; index < info->commandBufferCount; ++index)
            commands[index] = NativeHandle<VkCommandBuffer>(native->next++);
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Reset(VkCommandBuffer, VkCommandBufferResetFlags) {
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer, const VkCommandBufferBeginInfo *) {
        ++native->beginCalls;
        return native->beginResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer) {
        ++native->endCalls;
        return native->endResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *,
                                                          VkSemaphore *semaphore) {
        const auto *type = static_cast<const VkSemaphoreTypeCreateInfo *>(info->pNext);
        native->timelineOnly &= type != nullptr && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE && type->initialValue == 0;
        *semaphore = NativeHandle<VkSemaphore>(native->next++);
        native->counters[*semaphore] = 0;
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {
        ++native->destroyedSemaphores;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Submit(const VkQueue queue, const std::uint32_t count, const VkSubmitInfo2 *info, VkFence) {
        if (native->submitResult != VK_SUCCESS)
            return native->submitResult;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto &value = info[index];
            Submission captured{.queue = queue,
                                .command = value.pCommandBufferInfos[0].commandBuffer,
                                .signal = value.pSignalSemaphoreInfos[0]};
            if (value.waitSemaphoreInfoCount != 0)
                captured.waits.assign(value.pWaitSemaphoreInfos, value.pWaitSemaphoreInfos + value.waitSemaphoreInfoCount);
            native->accepted.push_back(std::move(captured));
        }
        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice, const VkSemaphore semaphore, std::uint64_t *value) {
        ++native->counterCalls;
        *value = native->counters.at(semaphore);
        return native->counterResult;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice, const VkSemaphoreWaitInfo *, std::uint64_t) {
        ++native->waitCalls;
        if (native->waitResult == VK_SUCCESS)
            native->Complete();
        return native->waitResult;
    }

    [[nodiscard]] inline PFN_vkVoidFunction Resolver(VkDevice, const char *name) {
        struct Entry {
            std::string_view name;
            PFN_vkVoidFunction function;
        };

        const std::array entries{Entry{"vkCreateCommandPool", EraseFunction(PFN_vkCreateCommandPool{CreatePool})},
                                 Entry{"vkDestroyCommandPool", EraseFunction(PFN_vkDestroyCommandPool{DestroyPool})},
                                 Entry{"vkAllocateCommandBuffers", EraseFunction(PFN_vkAllocateCommandBuffers{Allocate})},
                                 Entry{"vkResetCommandBuffer", EraseFunction(PFN_vkResetCommandBuffer{Reset})},
                                 Entry{"vkBeginCommandBuffer", EraseFunction(PFN_vkBeginCommandBuffer{Begin})},
                                 Entry{"vkEndCommandBuffer", EraseFunction(PFN_vkEndCommandBuffer{End})},
                                 Entry{"vkCreateSemaphore", EraseFunction(PFN_vkCreateSemaphore{CreateSemaphore})},
                                 Entry{"vkDestroySemaphore", EraseFunction(PFN_vkDestroySemaphore{DestroySemaphore})},
                                 Entry{"vkQueueSubmit2", EraseFunction(PFN_vkQueueSubmit2{Submit})},
                                 Entry{"vkGetSemaphoreCounterValue", EraseFunction(PFN_vkGetSemaphoreCounterValue{Counter})},
                                 Entry{"vkWaitSemaphores", EraseFunction(PFN_vkWaitSemaphores{Wait})}};
        const auto found = std::ranges::find(entries, std::string_view{name}, &Entry::name);
        return found == entries.end() ? nullptr : found->function;
    }

    struct Lease final : IRenderGraphResourceLease {
        int releases{};

        void Release() noexcept override {
            ++releases;
        }
    };

    /** @brief Explicit test owner keeps native dispatch state alive through any assertion unwind. */
    struct CommandFixture {
        Lease lease;
        Detail::VulkanCommandRetention pins;
        NativeState state;
        Detail::VulkanCommandRuntime runtime;
        const std::array<Detail::VulkanCommandQueue, 2> queues{
            {{{1}, 3, NativeHandle<VkQueue>(11), true}, {{2}, 7, NativeHandle<VkQueue>(12), false}}};

        CommandFixture() {
            native = &state;
        }

        ~CommandFixture() {
            state.Complete();
            state.counterResult = VK_SUCCESS;
            (void)runtime.BeginShutdown();
            (void)runtime.PollDrain();
            native = nullptr;
        }

        void Initialize(const std::uint32_t slots = 2, const std::uint64_t difference = 1024, const std::uint64_t incarnation = 11) {
            REQUIRE(runtime.Initialize(NativeHandle<VkDevice>(1), queues, slots, difference, incarnation, Resolver).HasValue());
        }

        [[nodiscard]] Detail::VulkanCommandTicket Record(const RenderQueueId queue = {1}) {
            const auto begun = runtime.Begin(queue);
            REQUIRE(begun.HasValue());
            REQUIRE(runtime.RecordingBuffer(begun.Value()).HasValue());
            REQUIRE(runtime.Finish(begun.Value()).HasValue());
            return begun.Value();
        }
    };
}  // namespace Horo::Render::VulkanTest
