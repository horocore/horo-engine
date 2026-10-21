#include "VulkanGraphRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        struct NativeState {
            VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
            VkAccessFlags2 access{VK_ACCESS_2_NONE};
            VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
            RenderGraphTextureLayout expected{RenderGraphTextureLayout::NotApplicable};
        };

        /** @brief Combines independent read/write masks without conflating access categories. */
        VkAccessFlags2 AccessMask(bool read, bool write, VkAccessFlags2 readMask, VkAccessFlags2 writeMask) {
            return (read ? readMask : 0) | (write ? writeMask : 0);
        }

        /** @brief Preserves transfer-only scope and exact source/destination layouts. */
        Result<NativeState> CopyState(const RenderGraphLogicalState &state) {
            if (state.scope != RenderGraphPipelineScope::Transfer)
                return Result<NativeState>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Copy access requires transfer scope."));
            const bool source = state.operation == RenderGraphSynchronizationOperation::CopySource;
            return Result<NativeState>::Success(
                {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, source ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 source ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 source ? RenderGraphTextureLayout::CopySource : RenderGraphTextureLayout::CopyDestination});
        }

        /** @brief Keeps sampled-image and storage access masks distinct. */
        NativeState ShaderState(const RenderGraphLogicalState &state, bool read, bool write) {
            const bool sampled = state.operation == RenderGraphSynchronizationOperation::Sampled;
            return {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                    sampled ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                            : AccessMask(read, write, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
                    sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                    sampled ? RenderGraphTextureLayout::ShaderReadOnly : RenderGraphTextureLayout::General};
        }

        /** @brief Maps declared operations to exact native masks and logical image layouts. */
        Result<NativeState> TranslateOperation(const RenderGraphLogicalState &state, bool read, bool write) {
            using enum RenderGraphSynchronizationOperation;
            switch (state.operation) {
                case CopySource:
                case CopyDestination:
                    return CopyState(state);
                case ColorAttachment:
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         AccessMask(read, write, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, RenderGraphTextureLayout::ColorAttachment});
                case DepthStencilAttachment:
                    return Result<NativeState>::Success(
                        {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         AccessMask(read, write, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, RenderGraphTextureLayout::DepthStencilAttachment});
                case GraphicsInputBuffer:
                    if (write)
                        return Result<NativeState>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
                    return Result<NativeState>::Success({VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_UNIFORM_READ_BIT |
                                                                                                   VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                                                                                   VK_ACCESS_2_INDEX_READ_BIT});
                case Sampled:
                case Storage:
                    return Result<NativeState>::Success(ShaderState(state, read, write));
                default:
                    return Result<NativeState>::Failure(
                        MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Unknown native graph operation."));
            }
        }

        /** @brief Validates the sole canonical undefined state. */
        Result<NativeState> UndefinedState(const RenderGraphLogicalState &state) {
            if (state.operation == RenderGraphSynchronizationOperation::None && state.scope == RenderGraphPipelineScope::External &&
                state.layout == RenderGraphTextureLayout::NotApplicable && !state.queue.IsValid())
                return Result<NativeState>::Success({});
            return Result<NativeState>::Failure(
                MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Malformed undefined native state."));
        }

        /** @brief Checks native buffer/image layout placement independently of operation mask selection. */
        bool ValidPlacement(const RenderGraphLogicalState &state, bool texture, const NativeState &native) {
            const bool transfer = state.operation == RenderGraphSynchronizationOperation::CopySource ||
                                  state.operation == RenderGraphSynchronizationOperation::CopyDestination;
            const bool inputBuffer = state.operation == RenderGraphSynchronizationOperation::GraphicsInputBuffer;
            return (texture ? state.layout == native.expected && !inputBuffer : state.layout == RenderGraphTextureLayout::NotApplicable) &&
                   (transfer || state.scope == RenderGraphPipelineScope::Graphics);
        }

        /** @brief Admits only native queue scopes implemented by this graphics/transfer command path. */
        bool ValidExecutionScope(const RenderGraphLogicalState &state) {
            return state.queue.IsValid() && state.scope != RenderGraphPipelineScope::External &&
                   state.scope != RenderGraphPipelineScope::Compute;
        }

        /** @brief Rejects unsupported scopes instead of emitting native masks unavailable on a queue. */
        [[nodiscard]] Result<NativeState> Translate(const RenderGraphLogicalState &state, const bool texture) {
            if (state.access == RenderGraphSynchronizationAccess::None)
                return UndefinedState(state);
            if (!ValidExecutionScope(state))
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
            if (!ValidPlacement(state, texture, translated.Value()))
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
