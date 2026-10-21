#include "VulkanGraphRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        struct NativeState {
            VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
            VkAccessFlags2 access{VK_ACCESS_2_NONE};
            VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
            RenderGraphTextureLayout expected{RenderGraphTextureLayout::NotApplicable};
        };

        /** @brief Maps declared operations to exact native masks and logical image layouts. */
        [[nodiscard]] Result<NativeState> TranslateOperation(const RenderGraphLogicalState &state, const bool read, const bool write) {
            using enum RenderGraphSynchronizationOperation;
            switch (state.operation) {
                case CopySource:
                case CopyDestination:
                    if (state.scope != RenderGraphPipelineScope::Transfer)
                        return Result<NativeState>::Failure(
                            MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Copy access requires transfer scope."));
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                         state.operation == CopySource ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         state.operation == CopySource ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         state.operation == CopySource ? RenderGraphTextureLayout::CopySource : RenderGraphTextureLayout::CopyDestination});
                case ColorAttachment:
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         (read ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0) | (write ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0),
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, RenderGraphTextureLayout::ColorAttachment});
                case DepthStencilAttachment:
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         (read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : 0) |
                             (write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0),
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, RenderGraphTextureLayout::DepthStencilAttachment});
                case Sampled:
                case Storage:
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                         state.operation == Sampled
                             ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                             : ((read ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : 0) | (write ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0)),
                         state.operation == Sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                         state.operation == Sampled ? RenderGraphTextureLayout::ShaderReadOnly : RenderGraphTextureLayout::General});
                default:
                    return Result<NativeState>::Failure(
                        MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Unknown native graph operation."));
            }
        }

        /** @brief Rejects unsupported scopes instead of emitting native masks unavailable on a queue. */
        [[nodiscard]] Result<NativeState> Translate(const RenderGraphLogicalState &state, const bool texture) {
            if (state.access == RenderGraphSynchronizationAccess::None) {
                if (state.operation == RenderGraphSynchronizationOperation::None && state.scope == RenderGraphPipelineScope::External &&
                    state.layout == RenderGraphTextureLayout::NotApplicable && !state.queue.IsValid())
                    return Result<NativeState>::Success({});
                return Result<NativeState>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Malformed undefined native state."));
            }
            if (!state.queue.IsValid() || state.scope == RenderGraphPipelineScope::External ||
                state.scope == RenderGraphPipelineScope::Compute)
                return Result<NativeState>::Failure(
                    MakeError(VulkanBackendErrors::UnsupportedOperation, "Native compute/external access scope is not admitted."));
            const bool read =
                state.access == RenderGraphSynchronizationAccess::Read || state.access == RenderGraphSynchronizationAccess::ReadWrite;
            const bool write =
                state.access == RenderGraphSynchronizationAccess::Write || state.access == RenderGraphSynchronizationAccess::ReadWrite;
            if (!read && !write)
                return Result<NativeState>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Unknown native graph access."));
            auto translated = TranslateOperation(state, read, write);
            if (translated.HasError())
                return translated;
            if ((texture && state.layout != translated.Value().expected) ||
                (!texture && state.layout != RenderGraphTextureLayout::NotApplicable) ||
                (state.operation != RenderGraphSynchronizationOperation::CopySource &&
                 state.operation != RenderGraphSynchronizationOperation::CopyDestination &&
                 state.scope != RenderGraphPipelineScope::Graphics))
                return Result<NativeState>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Native graph layout/scope differs from declared operation."));
            return translated;
        }

        [[nodiscard]] VkImageAspectFlags Aspects(const RenderTextureFormat format) noexcept {
            switch (format) {
                case RenderTextureFormat::Depth24Stencil8:
                case RenderTextureFormat::Depth32FloatStencil8:
                    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                case RenderTextureFormat::Depth16Unorm:
                case RenderTextureFormat::Depth32Float:
                    return VK_IMAGE_ASPECT_DEPTH_BIT;
                default:
                    return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

        /** @brief Builds the whole-image dependency while retaining the matched old/new layout pair. */
        [[nodiscard]] VkImageMemoryBarrier2 ImageBarrier(const VulkanGraphResource &resource, const NativeState &source,
                                                         const NativeState &destination, const VkImageLayout oldLayout,
                                                         const VkImageLayout newLayout, const std::uint32_t sourceFamily,
                                                         const std::uint32_t destinationFamily) noexcept {
            return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = source.stages,
                    .srcAccessMask = source.access,
                    .dstStageMask = destination.stages,
                    .dstAccessMask = destination.access,
                    .oldLayout = oldLayout,
                    .newLayout = newLayout,
                    .srcQueueFamilyIndex = sourceFamily,
                    .dstQueueFamilyIndex = destinationFamily,
                    .image = resource.image,
                    .subresourceRange = {.aspectMask = Aspects(resource.textureDescriptor.format),
                                         .baseMipLevel = 0,
                                         .levelCount = resource.textureDescriptor.mipCount,
                                         .baseArrayLayer = 0,
                                         .layerCount = resource.textureDescriptor.layerCount}};
        }

        /** @brief Builds the exact whole-buffer dependency without a temporary native allocation. */
        [[nodiscard]] VkBufferMemoryBarrier2 BufferBarrier(const VulkanGraphResource &resource, const NativeState &source,
                                                           const NativeState &destination, const std::uint32_t sourceFamily,
                                                           const std::uint32_t destinationFamily) noexcept {
            return {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = source.stages,
                    .srcAccessMask = source.access,
                    .dstStageMask = destination.stages,
                    .dstAccessMask = destination.access,
                    .srcQueueFamilyIndex = sourceFamily,
                    .dstQueueFamilyIndex = destinationFamily,
                    .buffer = resource.buffer,
                    .offset = 0,
                    .size = VK_WHOLE_SIZE};
        }
    }  // namespace

    /** @copydoc TranslateVulkanGraphBarrier */
    Result<VulkanGraphBarrier> TranslateVulkanGraphBarrier(const VulkanGraphResource &resource, const RenderGraphLogicalState &before,
                                                           const RenderGraphLogicalState &after, const std::uint32_t sourceFamily,
                                                           const std::uint32_t destinationFamily, const bool release, const bool acquire,
                                                           const bool discardPriorAccess) {
        const bool texture = resource.kind == RenderGraphResourceKind::Texture;
        auto oldState = Translate(before, texture);
        if (oldState.HasError())
            return Result<VulkanGraphBarrier>::Failure(std::move(oldState).ErrorValue());
        auto newState = Translate(after, texture);
        if (newState.HasError())
            return Result<VulkanGraphBarrier>::Failure(std::move(newState).ErrorValue());
        auto source = oldState.Value();
        auto destination = newState.Value();
        // Discarding contents still orders all previous accesses on the same effective queue.
        if (discardPriorAccess) {
            source.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            source.access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
        if (release) {
            destination.stages = VK_PIPELINE_STAGE_2_NONE;
            destination.access = VK_ACCESS_2_NONE;
        }
        if (acquire) {
            source.stages = VK_PIPELINE_STAGE_2_NONE;
            source.access = VK_ACCESS_2_NONE;
        }
        VulkanGraphBarrier barrier;
        barrier.texture = texture;
        if (texture)
            barrier.image = ImageBarrier(resource, source, destination, oldState.Value().layout, newState.Value().layout, sourceFamily,
                                         destinationFamily);
        else
            barrier.buffer = BufferBarrier(resource, source, destination, sourceFamily, destinationFamily);
        return Result<VulkanGraphBarrier>::Success(barrier);
    }
}  // namespace Horo::Render::Detail
