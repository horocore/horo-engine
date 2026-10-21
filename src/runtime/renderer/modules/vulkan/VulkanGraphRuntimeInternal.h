#pragma once

#include "VulkanGraphRuntime.h"
#include "VulkanRenderBackendErrors.h"

#include <array>
#include <thread>

namespace Horo::Render::Detail {
    /** @brief One prevalidated whole-resource dependency; release/acquire masks differ but layouts match. */
    struct VulkanGraphBarrier {
        VkBufferMemoryBarrier2 buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        VkImageMemoryBarrier2 image{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        bool texture{false};
    };

    /** @brief Creates one native dependency without native side effects. */
    [[nodiscard]] Result<VulkanGraphBarrier> TranslateVulkanGraphBarrier(const VulkanGraphResource &resource,
                                                                         const RenderGraphLogicalState &before,
                                                                         const RenderGraphLogicalState &after, std::uint32_t sourceFamily,
                                                                         std::uint32_t destinationFamily, bool release, bool acquire,
                                                                         bool discardPriorAccess);

    struct VulkanGraphRuntime::Impl {
        struct Pass {
            RenderGraphExecutionRange before;
            RenderGraphExecutionRange after;
        };

        struct Update {
            VulkanGraphResourceState *target{};
            VulkanGraphResourceState value;
        };

        struct Submission {
            VulkanCommandTicket ticket;
            RenderQueueSubmission value;
            std::array<RenderTimelinePoint, RenderGraphSubmissionLimits::MaximumQueues> waits;
            RenderGraphExecutionRange passes;
            RenderGraphExecutionRange updates;
        };

        struct Dispatch {
            PFN_vkCmdPipelineBarrier2 barrier{};
            PFN_vkCmdCopyBuffer2 copy{};
            PFN_vkCmdBeginRendering beginRendering{};
            PFN_vkCmdEndRendering endRendering{};
        } native;

        VulkanCommandRuntime commands;
        VulkanResourceRuntime *resources{};
        std::thread::id owner;
        std::vector<VulkanCommandQueue> queues;
        std::vector<VulkanGraphResource> bindings;
        std::vector<VulkanGraphResourceState> states;
        std::vector<const VulkanGraphResourceState *> identities;
        std::vector<bool> used;
        std::vector<bool> discarded;
        std::vector<Pass> passes;
        std::vector<VulkanGraphBarrier> barriers;
        std::vector<Update> updates;
        std::vector<Submission> submissions;
        std::vector<VulkanCommandRetention> retention;
        FrameToken frame;
        bool recorded{false};
        bool closing{false};
        bool failedSubmission{false};

        [[nodiscard]] Result<void> Access(bool admission = true) const;
        [[nodiscard]] const VulkanCommandQueue *Queue(RenderQueueId id) const noexcept;
        [[nodiscard]] Result<void> Prepare(const RenderGraphExecutionRequest &request);
        [[nodiscard]] Result<void> InitialStates(const RenderGraphExecutionRequest &request);
        [[nodiscard]] Result<void> PreparePass(const RenderGraphExecutionRequest &request, std::size_t index);
        [[nodiscard]] Result<void> PrepareTransitions(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                      bool releases);
        [[nodiscard]] Result<void> ValidateWorkload(const RenderGraphWorkload &workload, RenderQueueId queue);
        [[nodiscard]] Result<void> Record(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins);
        void Encode(VkCommandBuffer command, const RenderGraphWorkload &workload) const noexcept;
        void Barriers(VkCommandBuffer command, RenderGraphExecutionRange range) const noexcept;
        void Abort() noexcept;

        [[nodiscard]] static Error Invalid(const char *reason) {
            return MakeError(VulkanBackendErrors::ResourceRequestInvalid, reason);
        }

        [[nodiscard]] static Error Unsupported(const char *reason) {
            return MakeError(VulkanBackendErrors::UnsupportedOperation, reason);
        }
    };
}  // namespace Horo::Render::Detail
