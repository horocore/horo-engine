#include "VulkanGraphRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        struct NativeState {
            VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
            VkAccessFlags2 access{VK_ACCESS_2_NONE};
            VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        };

        /** @brief Rejects unsupported scopes instead of emitting native stage masks unavailable on a queue. */
        [[nodiscard]] Result<NativeState> Translate(const RenderGraphLogicalState &state, const bool texture) {
            NativeState native;
            if (state.access == RenderGraphSynchronizationAccess::None) {
                if (state.operation == RenderGraphSynchronizationOperation::None && state.scope == RenderGraphPipelineScope::External &&
                    state.layout == RenderGraphTextureLayout::NotApplicable && !state.queue.IsValid())
                    return Result<NativeState>::Success(native);
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
            RenderGraphTextureLayout expected{RenderGraphTextureLayout::NotApplicable};
            using enum RenderGraphSynchronizationOperation;
            switch (state.operation) {
                case CopySource:
                case CopyDestination:
                    if (state.scope != RenderGraphPipelineScope::Transfer)
                        return Result<NativeState>::Failure(
                            MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Copy access requires transfer scope."));
                    native.stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    native.access = state.operation == CopySource ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    native.layout =
                        state.operation == CopySource ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    expected =
                        state.operation == CopySource ? RenderGraphTextureLayout::CopySource : RenderGraphTextureLayout::CopyDestination;
                    break;
                case ColorAttachment:
                    native.stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    native.access =
                        (read ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0) | (write ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0);
                    native.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    expected = RenderGraphTextureLayout::ColorAttachment;
                    break;
                case DepthStencilAttachment:
                    native.stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                    native.access = (read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : 0) |
                                    (write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
                    native.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    expected = RenderGraphTextureLayout::DepthStencilAttachment;
                    break;
                case Sampled:
                case Storage:
                    native.stages = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                    native.access =
                        state.operation == Sampled
                            ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                            : ((read ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : 0) | (write ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0));
                    native.layout = state.operation == Sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
                    expected = state.operation == Sampled ? RenderGraphTextureLayout::ShaderReadOnly : RenderGraphTextureLayout::General;
                    break;
                default:
                    return Result<NativeState>::Failure(
                        MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Unknown native graph operation."));
            }
            if ((texture && state.layout != expected) || (!texture && state.layout != RenderGraphTextureLayout::NotApplicable) ||
                (state.operation != CopySource && state.operation != CopyDestination && state.scope != RenderGraphPipelineScope::Graphics))
                return Result<NativeState>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Native graph layout/scope differs from declared operation."));
            return Result<NativeState>::Success(native);
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
        if (texture) {
            barrier.image.srcStageMask = source.stages;
            barrier.image.srcAccessMask = source.access;
            barrier.image.dstStageMask = destination.stages;
            barrier.image.dstAccessMask = destination.access;
            barrier.image.oldLayout = oldState.Value().layout;
            barrier.image.newLayout = newState.Value().layout;
            barrier.image.srcQueueFamilyIndex = sourceFamily;
            barrier.image.dstQueueFamilyIndex = destinationFamily;
            barrier.image.image = resource.image;
            barrier.image.subresourceRange = {.aspectMask = Aspects(resource.textureDescriptor.format),
                                              .baseMipLevel = 0,
                                              .levelCount = resource.textureDescriptor.mipCount,
                                              .baseArrayLayer = 0,
                                              .layerCount = resource.textureDescriptor.layerCount};
        } else {
            barrier.buffer.srcStageMask = source.stages;
            barrier.buffer.srcAccessMask = source.access;
            barrier.buffer.dstStageMask = destination.stages;
            barrier.buffer.dstAccessMask = destination.access;
            barrier.buffer.srcQueueFamilyIndex = sourceFamily;
            barrier.buffer.dstQueueFamilyIndex = destinationFamily;
            barrier.buffer.buffer = resource.buffer;
            barrier.buffer.offset = 0;
            barrier.buffer.size = VK_WHOLE_SIZE;
        }
        return Result<VulkanGraphBarrier>::Success(barrier);
    }
}  // namespace Horo::Render::Detail
