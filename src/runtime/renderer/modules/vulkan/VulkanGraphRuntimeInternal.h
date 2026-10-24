#pragma once

#include "VulkanGraphRuntime.h"
#include "VulkanMultiviewRuntime.h"
#include "VulkanRenderBackendErrors.h"

#include <array>
#include <bit>
#include <thread>
#include <type_traits>

namespace Horo::Render::Detail {
    /** @brief One prevalidated whole-resource dependency; release/acquire masks differ but layouts match. */
    struct VulkanGraphBarrier {
        VkBufferMemoryBarrier2 buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        VkImageMemoryBarrier2 image{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        bool texture{false};
    };

    /** @brief Exact native ownership role and discarded-access policy for a single barrier. */
    struct VulkanGraphBarrierOptions {
        std::uint32_t sourceFamily{VK_QUEUE_FAMILY_IGNORED};
        std::uint32_t destinationFamily{VK_QUEUE_FAMILY_IGNORED};
        bool release{false};
        bool acquire{false};
        bool discardPriorAccess{false};
    };

    /** @brief Creates one native dependency without native side effects. */
    [[nodiscard]] Result<VulkanGraphBarrier> TranslateVulkanGraphBarrier(const VulkanGraphResource &resource,
                                                                         const RenderGraphLogicalState &before,
                                                                         const RenderGraphLogicalState &after,
                                                                         const VulkanGraphBarrierOptions &options = {});

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
            VkSemaphore presentationSignal{};
            RenderQueueSubmission value;
            std::array<RenderTimelinePoint, RenderGraphSubmissionLimits::MaximumQueues> waits;
            RenderGraphExecutionRange passes;
            RenderGraphExecutionRange updates;
        };

        struct Dispatch {
            PFN_vkCmdPipelineBarrier2 barrier{};
            PFN_vkCmdCopyBuffer2 copy{};
            PFN_vkCmdCopyBufferToImage2 copyImage{};
            PFN_vkCmdBeginRendering beginRendering{};
            PFN_vkCmdEndRendering endRendering{};
            PFN_vkCmdBindPipeline bindPipeline{};
            PFN_vkCmdBindDescriptorSets bindSets{};
            PFN_vkCmdBindVertexBuffers bindVertices{};
            PFN_vkCmdBindIndexBuffer bindIndex{};
            PFN_vkCmdSetViewport viewport{};
            PFN_vkCmdSetScissor scissor{};
            PFN_vkCmdDrawIndexed drawIndexed{};
        };

        Dispatch native;

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

        /** @brief Active-frame admission and terminal submission/shutdown state, owned together by the command runtime. */
        struct Lifecycle {
            FrameToken frame;
            VulkanCommandRetention *pins{}; /**< Same canonical retained owner shared by viewport and later GUI tail. */
            bool recorded{false};
            bool closing{false};
            bool failedSubmission{false};
        };

        Lifecycle lifecycle;
        std::array<VulkanGuiSample, GuiCompositionPreparation::MaximumResources> guiSamples;
        std::array<VulkanGuiBuffer, GuiCompositionPreparation::MaximumResources> guiBuffers;

        struct GuiSamplePlan {
            RenderGraphExecutionRange before;
            RenderGraphExecutionRange after;
            bool upload{};
        };

        std::array<GuiSamplePlan, GuiCompositionPreparation::MaximumResources> guiPlans;
        RenderGraphExecutionRange guiBufferBarriers;
        RenderGraphExecutionRange guiExternalFinalBarriers;
        std::optional<RenderQueueSubmission> guiSubmission;
        std::array<RenderTimelinePoint, RenderGraphSubmissionLimits::MaximumQueues> guiWaits;
        std::array<GuiSampledResourceInstance, GuiCompositionPreparation::MaximumResources> guiIdentities;
        std::array<std::uint64_t, GuiCompositionPreparation::MaximumResources> guiBufferIdentities;
        std::size_t guiSampleCount{};
        std::size_t guiBufferCount{};
        std::array<VulkanGuiSampleBinding, GuiCompositionPreparation::MaximumResources> guiSampleBindings;
        std::array<VulkanGuiBufferBinding, GuiCompositionPreparation::MaximumResources> guiNativeBuffers;
        bool guiPrepared{};
        bool guiRecorded{};

        /** @brief Adds a GUI state transition using earlier recorded producers without publishing unsent state. */
        [[nodiscard]] Result<void> GuiState(const VulkanGraphResource &resource, RenderGraphLogicalState next,
                                            const RenderQueueSubmission &submission, bool requireContents);
        /** @brief Reads the most recent pending state, falling back to the canonical accepted state. */
        [[nodiscard]] const VulkanGraphResourceState &PendingState(const VulkanGraphResource &resource) const;
        /** @brief Encodes prepared uploads and whole-output GUI rendering into a borrowed canonical command. */
        [[nodiscard]] Result<void> EncodeGui(VkCommandBuffer command, const GuiCompositionRequest &request, const VulkanGuiOutput &output,
                                             IVulkanGuiEncoder &encoder);
        /** @brief Prepares bounded native GUI resources, with exact upload-before-sample ordering. */
        /** @brief Pins exact input buffers and synthesizes their current canonical tail barriers. */
        [[nodiscard]] Result<void> PrepareGuiBuffers(const GuiCompositionRequest &request);
        [[nodiscard]] Result<void> PrepareGuiResources(const GuiCompositionRequest &request);

        /** @brief Borrows the synchronous resolver to load optional material dispatch without copying or allocation. */
        template <typename Resolver> void LoadMaterialDispatch(VkDevice device, const Resolver &resolver) {
            if constexpr (std::is_null_pointer_v<Resolver>) {
                return;
            } else {
                if constexpr (std::is_pointer_v<Resolver>) {
                    if (resolver == nullptr)
                        return;
                }
                native.bindPipeline = std::bit_cast<PFN_vkCmdBindPipeline>(resolver(device, "vkCmdBindPipeline"));
                native.bindSets = std::bit_cast<PFN_vkCmdBindDescriptorSets>(resolver(device, "vkCmdBindDescriptorSets"));
                native.bindVertices = std::bit_cast<PFN_vkCmdBindVertexBuffers>(resolver(device, "vkCmdBindVertexBuffers"));
                native.bindIndex = std::bit_cast<PFN_vkCmdBindIndexBuffer>(resolver(device, "vkCmdBindIndexBuffer"));
                native.viewport = std::bit_cast<PFN_vkCmdSetViewport>(resolver(device, "vkCmdSetViewport"));
                native.scissor = std::bit_cast<PFN_vkCmdSetScissor>(resolver(device, "vkCmdSetScissor"));
                native.drawIndexed = std::bit_cast<PFN_vkCmdDrawIndexed>(resolver(device, "vkCmdDrawIndexed"));
            }
        }

        [[nodiscard]] Result<void> Access(bool admission = true) const;
        [[nodiscard]] const VulkanCommandQueue *Queue(RenderQueueId id) const noexcept;
        [[nodiscard]] Result<void> Prepare(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins);
        [[nodiscard]] Result<void> InitialStates(const RenderGraphExecutionRequest &request);
        [[nodiscard]] Result<void> PreparePass(const RenderGraphExecutionRequest &request, std::size_t index, VulkanCommandRetention &pins);
        /** @brief Appends one exact transition and commits only its scratch logical state. */
        [[nodiscard]] Result<void> AppendTransition(const RenderGraphTransition &transition, bool release, bool acquire);
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
        /** @brief Records one previously admitted finite native buffer copy. */
        void EncodeCopy(VkCommandBuffer command, const RenderGraphBufferCopy &operation) const noexcept;
        /** @brief Records one previously admitted whole-array native color attachment. */
        void EncodeColor(VkCommandBuffer command, const RenderGraphColorAttachment &operation) const noexcept;
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
