#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Transitions only the acquired WSI image; presentation ownership already proved acquisition completion. */
        void OutputBarrier(PFN_vkCmdPipelineBarrier2 barrier, VkCommandBuffer command, VkImage image, bool present) {
            const VkImageMemoryBarrier2 imageBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                                     .srcStageMask = present ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                                             : VK_PIPELINE_STAGE_2_NONE,
                                                     .srcAccessMask = present ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE,
                                                     .dstStageMask = present ? VK_PIPELINE_STAGE_2_NONE
                                                                             : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     .dstAccessMask = present ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                     .oldLayout =
                                                         present ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                                     .newLayout = present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                                          : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                     .image = image,
                                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .imageMemoryBarrierCount = 1,
                                              .pImageMemoryBarriers = &imageBarrier};
            barrier(command, &dependency);
        }
    }  // namespace

    /** @copydoc VulkanGraphRuntime::Impl::EncodeGui */
    Result<void> VulkanGraphRuntime::Impl::EncodeGui(const VkCommandBuffer command, const GuiCompositionRequest &request,
                                                     const VulkanGuiOutput &output, IVulkanGuiEncoder &encoder) {
        Barriers(command, guiBufferBarriers);
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto &sample = guiSamples[index];
            const auto &plan = guiPlans[index];
            Barriers(command, plan.before);
            if (plan.upload) {
                const auto &texture = sample.texture.textureDescriptor;
                const VkBufferImageCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                .imageExtent = {texture.extent.width, texture.extent.height, 1}};
                const VkCopyBufferToImageInfo2 copy{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                                                    .srcBuffer = sample.staging.buffer,
                                                    .dstImage = sample.texture.image,
                                                    .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                    .regionCount = 1,
                                                    .pRegions = &region};
                native.copyImage(command, &copy);
            }
            Barriers(command, plan.after);
        }
        // SYNC: The acquired image is discarded, then transitioned to presentation after this sole GUI encoder.
        OutputBarrier(native.barrier, command, output.image, false);
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = output.view,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {{output.clearColor.red, output.clearColor.green,
                                                                        output.clearColor.blue, output.clearColor.alpha}}}};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {output.extent.width, output.extent.height}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color};
        native.beginRendering(command, &rendering);
        const VulkanFrameRetentionSlot slot{static_cast<std::uint32_t>(lifecycle.pins - retention.data()),
                                            static_cast<std::uint32_t>(retention.size())};
        const auto encoded = encoder.Record(command, slot, std::span{guiSampleBindings}.first(request.resources.size()),
                                            std::span{guiNativeBuffers}.first(request.buffers.size()));
        native.endRendering(command);
        if (encoded.HasError())
            return encoded;
        Barriers(command, guiExternalFinalBarriers);
        OutputBarrier(native.barrier, command, output.image, true);
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::ValidateGuiSubmission */
    Result<void> VulkanGraphRuntime::Impl::ValidateGuiSubmission(const GuiCompositionRequest &request,
                                                                 const VulkanGuiOutput &output) const {
        if (const auto *pins = lifecycle.pins; request.frame != lifecycle.frame || !guiPrepared || guiRecorded || !request.submission ||
                                               pins == nullptr || request.lease != pins->lease || !output.image || !output.view ||
                                               !output.renderFinished || !output.extent.IsValid())
            return Result<void>::Failure(Invalid("GUI encoding requires the exact admitted frame, lease and acquired WSI output."));
        const auto &admitted = *guiSubmission;
        const auto &submissionIdentity = *request.submission;
        const auto equalSample = [](const GuiSampledResourceInstance &left, const GuiSampledResourceInstance &right) {
            return left.view == right.view && left.sampler == right.sampler && left.uploadSource == right.uploadSource;
        };
        if (submissionIdentity.queue != admitted.queue || submissionIdentity.order != admitted.order ||
            submissionIdentity.signal != admitted.signal || !std::ranges::equal(submissionIdentity.waits, admitted.waits) ||
            !std::ranges::equal(request.resources, std::span{guiIdentities}.first(guiSampleCount), equalSample) ||
            !std::ranges::equal(request.buffers, std::span{guiBufferIdentities}.first(guiBufferCount)))
            return Result<void>::Failure(Invalid("GUI encoder changed its admitted submission or resource identities."));

        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::GuiPreparationRollback::~GuiPreparationRollback */
    VulkanGraphRuntime::Impl::GuiPreparationRollback::~GuiPreparationRollback() noexcept {
        if (committed)
            return;
        graph.updates.erase(graph.updates.begin() + updateCount, graph.updates.end());
        graph.barriers.erase(graph.barriers.begin() + barrierCount, graph.barriers.end());
        pins.resources.erase(pins.resources.begin() + resourceCount, pins.resources.end());
        graph.guiBufferBarriers = bufferBarriers;
        graph.guiExternalFinalBarriers = externalBarriers;
        // Per-sample scratch is overwritten completely on retry and is never read before successful preparation.
    }

    /** @copydoc VulkanGraphRuntime::ExecuteGui */
    Result<void> VulkanGraphRuntime::ExecuteGui(const GuiCompositionRequest &request, const VulkanGuiOutput &output,
                                                IVulkanGuiEncoder &encoder) {
        if (const auto access = impl_->Access(); access.HasError())
            return access;
        if (auto admitted = impl_->ValidateGuiSubmission(request, output); admitted.HasError())
            return admitted;
        auto *pins = impl_->lifecycle.pins;
        // Preparing resources may append upload state before a later sample/output/ticket is rejected.
        // Keep the earlier scene's exact state and retained owners; only this unsent attempt rolls back.
        Impl::GuiPreparationRollback rollback{*impl_,
                                              *pins,
                                              impl_->updates.size(),
                                              impl_->barriers.size(),
                                              pins->resources.size(),
                                              impl_->guiBufferBarriers,
                                              impl_->guiExternalFinalBarriers};

        const auto updateOffset = rollback.updateCount;
        if (const auto prepared = impl_->PrepareGuiResources(request); prepared.HasError())
            return prepared;
        if (!output.legacyUnmanagedOutput &&
            std::ranges::any_of(std::span{impl_->guiSamples}.first(request.resources.size()), [](const VulkanGuiSample &sample) {
            return sample.requiredFinal.has_value();
        }))
            return Result<void>::Failure(
                Impl::Unsupported("XR first-eye mirror requires the explicitly admitted legacy unmanaged output."));
        const auto ticket = impl_->commands.Begin(request.submission->queue);
        if (ticket.HasError())
            return Result<void>::Failure(ticket.ErrorValue());
        // This ticket now belongs to the canonical abort/submission path, which retains all pending native debt.
        rollback.committed = true;
        impl_->submissions.emplace_back();
        auto &submission = impl_->submissions.back();
        submission.ticket = ticket.Value();
        submission.presentationSignal = output.renderFinished;
        submission.value = *request.submission;
        std::ranges::copy(request.submission->waits, submission.waits.begin());
        submission.value.waits = std::span{submission.waits}.first(request.submission->waits.size());
        submission.updates = {updateOffset, impl_->updates.size() - updateOffset};
        if (const auto retained = impl_->commands.Retain(submission.ticket, *pins); retained.HasError())
            return retained;
        const auto command = impl_->commands.RecordingBuffer(submission.ticket);
        if (command.HasError())
            return Result<void>::Failure(command.ErrorValue());
        if (const auto encoded = impl_->EncodeGui(command.Value(), request, output, encoder); encoded.HasError())
            return encoded;
        if (const auto finished = impl_->commands.Finish(submission.ticket); finished.HasError())
            return finished;
        impl_->guiRecorded = true;
        impl_->lifecycle.recorded = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
