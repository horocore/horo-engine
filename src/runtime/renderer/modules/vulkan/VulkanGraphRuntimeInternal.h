#pragma once

#include "VulkanGraphRuntime.h"
#include "VulkanMultiviewRuntime.h"
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
        /** @brief Reserves the fixed graph and canonical retention envelope before any frame admission. */
        [[nodiscard]] Result<void> ReserveStorage(std::span<const VulkanCommandQueue> queues, std::uint32_t frames);

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
            PFN_vkCmdBindPipeline bindPipeline{};
            PFN_vkCmdBindDescriptorSets bindSets{};
            PFN_vkCmdBindVertexBuffers bindVertices{};
            PFN_vkCmdBindIndexBuffer bindIndex{};
            PFN_vkCmdSetViewport viewport{};
            PFN_vkCmdSetScissor scissor{};
            PFN_vkCmdDrawIndexed drawIndexed{};
        } native;

        struct MaterialDraw {
            VulkanMaterialMesh mesh;
            const VulkanPreparedMaterialBinding *material{};
        };

        std::array<MaterialDraw, RenderMaterialPass::MaximumDraws> materialDraws;
        VulkanMaterialTarget materialTarget;
        std::uint64_t deviceIncarnation{};
        VulkanCommandRuntime commands;
        VulkanMultiviewRecorder multiview;
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

        void LoadMaterialDispatch(VkDevice device, const VulkanShaderRuntime::Resolver &resolver);
        [[nodiscard]] Result<void> Access(bool admission = true) const;
        [[nodiscard]] const VulkanCommandQueue *Queue(RenderQueueId id) const noexcept;
        [[nodiscard]] Result<void> Prepare(const RenderGraphExecutionRequest &request, VulkanCommandRetention &retention);
        [[nodiscard]] Result<void> InitialStates(const RenderGraphExecutionRequest &request);
        [[nodiscard]] Result<void> PreparePass(const RenderGraphExecutionRequest &request, std::size_t index,
                                               VulkanCommandRetention &retention);
        [[nodiscard]] Result<void> PrepareTransitions(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                      bool releases);
        /** @brief Validates exact foreign acquisition/final-state obligations before generic state initialization. */
        [[nodiscard]] Result<void> ValidateExternalAcquisition(const RenderGraphExecutionRequest &request, std::size_t index) const;
        /** @brief Prepares native post-producer release barriers from exact external acquisition evidence. */
        [[nodiscard]] Result<void> PrepareExternalReleases(const RenderGraphExecutionRequest &request,
                                                           const RenderGraphExecutionPass &pass);
        /** @brief Validates the whole-array native depth attachment before recording. */
        [[nodiscard]] Result<void> ValidateDepthAttachment(const RenderGraphDepthAttachment &depth, RenderQueueId queue);
        /** @brief Encodes the admitted normalized depth clear without allocation. */
        void EncodeDepthAttachment(VkCommandBuffer command, const RenderGraphDepthAttachment &depth) const noexcept;
        [[nodiscard]] Result<void> ValidateWorkload(const RenderGraphWorkload &workload, RenderQueueId queue);
        [[nodiscard]] Result<void> Record(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins);
        void Encode(VkCommandBuffer command, const RenderGraphWorkload &workload) const noexcept;
        void Barriers(VkCommandBuffer command, RenderGraphExecutionRange range) const noexcept;
        [[nodiscard]] Result<void> ValidateMaterialAdmission(const RenderMaterialExecutionRequest &request) const;
        [[nodiscard]] Result<void> RecordMaterial(const RenderMaterialExecutionRequest &request, VulkanCommandRetention &pins);
        [[nodiscard]] Result<void> PrepareMaterialTarget(const RenderMaterialExecutionRequest &request, VulkanCommandRetention &pins);
        [[nodiscard]] Result<void> PrepareMaterialDraw(const RenderMaterialDrawInstance &source, MaterialDraw &draw,
                                                       const RenderQueueSubmission &submission, VulkanCommandRetention &pins);
        [[nodiscard]] Result<void> PrepareMaterialResources(const VulkanPreparedMaterialBinding &material,
                                                            const RenderQueueSubmission &submission);
        [[nodiscard]] Result<void> PrepareMaterial(const RenderMaterialExecutionRequest &request, VulkanCommandRetention &pins);
        [[nodiscard]] Result<void> MaterialState(const VulkanGraphResource &resource, RenderGraphLogicalState next,
                                                 const RenderQueueSubmission &submission, bool requireContents);
        void EncodeMaterial(VkCommandBuffer command, const RenderMaterialExecutionRequest &request) const noexcept;
        void Abort() noexcept;

        [[nodiscard]] static Error Invalid(const char *reason) {
            return MakeError(VulkanBackendErrors::ResourceRequestInvalid, reason);
        }

        [[nodiscard]] static Error Unsupported(const char *reason) {
            return MakeError(VulkanBackendErrors::UnsupportedOperation, reason);
        }
    };
}  // namespace Horo::Render::Detail
