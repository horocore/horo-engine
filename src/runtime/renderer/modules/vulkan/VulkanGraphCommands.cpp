#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    /** @brief Validates native descriptor roles and contents before recording; unsupported adjacent capabilities remain explicit. */
    Result<void> VulkanGraphRuntime::Impl::ValidateWorkload(const RenderGraphWorkload &workload, const RenderQueueId queue) {
        if (std::holds_alternative<std::monostate>(workload))
            return Result<void>::Success();
        if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            const auto &source = bindings[copy->source.value - 1];
            const auto &destination = bindings[copy->destination.value - 1];
            const auto fits = [](const std::size_t offset, const std::size_t count, const std::size_t size) {
                return offset <= size && count <= size - offset;
            };
            if (source.kind != RenderGraphResourceKind::Buffer || destination.kind != RenderGraphResourceKind::Buffer ||
                source.buffer == VK_NULL_HANDLE || destination.buffer == VK_NULL_HANDLE || source.buffer == destination.buffer ||
                copy->byteCount == 0 || !HasBufferUsage(source.bufferDescriptor.usage, RenderBufferUsage::CopySource) ||
                !HasBufferUsage(destination.bufferDescriptor.usage, RenderBufferUsage::CopyDestination) ||
                !fits(copy->sourceOffset, copy->byteCount, source.bufferDescriptor.byteSize) ||
                !fits(copy->destinationOffset, copy->byteCount, destination.bufferDescriptor.byteSize))
                return Result<void>::Failure(Invalid("Buffer copy differs from native buffer identity, usage or checked byte bounds."));
            if (!states[copy->source.value - 1].contentsDefined)
                return Result<void>::Failure(Invalid("Buffer copy source contents are not defined."));
            if (copy->destinationOffset == 0 && copy->byteCount == destination.bufferDescriptor.byteSize)
                states[copy->destination.value - 1].contentsDefined = true;
            return Result<void>::Success();
        }
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            const auto &binding = bindings[color->texture.value - 1];
            const auto *nativeQueue = Queue(queue);
            if (nativeQueue == nullptr || !nativeQueue->supportsGraphics || binding.colorView == VK_NULL_HANDLE ||
                binding.kind != RenderGraphResourceKind::Texture || !color->operations.IsValid() ||
                color->operations.storeOperation != AttachmentStoreOperation::Store ||
                color->operations.loadOperation == AttachmentLoadOperation::DontCare)
                return Result<void>::Failure(Unsupported(
                    "Color work requires an admitted graphics queue and whole single-sample color attachment with preserved store."));
            auto &state = states[color->texture.value - 1];
            if (color->operations.loadOperation == AttachmentLoadOperation::Load && !state.contentsDefined)
                return Result<void>::Failure(Invalid("Color attachment Load requires defined native contents."));
            state.contentsDefined = true;
            return Result<void>::Success();
        }
        if (std::holds_alternative<PrimaryOutputAttachment>(workload))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported,
                                                   "Primary output acquisition/presentation belongs to the Vulkan WSI capability."));
        return Result<void>::Failure(
            Unsupported("The selected Vulkan command path does not realize shader, light-culling or VFX workloads."));
    }

    /** @brief Emits exactly one prepared dependency at a time without scratch allocation. */
    void VulkanGraphRuntime::Impl::Barriers(const VkCommandBuffer command, const RenderGraphExecutionRange range) const noexcept {
        for (const auto &barrier : std::span{barriers}.subspan(range.offset, range.count)) {
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .bufferMemoryBarrierCount = barrier.texture ? 0U : 1U,
                                              .pBufferMemoryBarriers = barrier.texture ? nullptr : &barrier.buffer,
                                              .imageMemoryBarrierCount = barrier.texture ? 1U : 0U,
                                              .pImageMemoryBarriers = barrier.texture ? &barrier.image : nullptr};
            native.barrier(command, &dependency);
        }
    }

    /** @brief Encodes only a workload already proven against exact resident descriptors. */
    void VulkanGraphRuntime::Impl::Encode(const VkCommandBuffer command, const RenderGraphWorkload &workload) const noexcept {
        if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            const VkBufferCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                                       .srcOffset = copy->sourceOffset,
                                       .dstOffset = copy->destinationOffset,
                                       .size = copy->byteCount};
            const VkCopyBufferInfo2 operation{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                                              .srcBuffer = bindings[copy->source.value - 1].buffer,
                                              .dstBuffer = bindings[copy->destination.value - 1].buffer,
                                              .regionCount = 1,
                                              .pRegions = &region};
            native.copy(command, &operation);
        } else if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            const auto &binding = bindings[color->texture.value - 1];
            const auto &clear = color->operations.clearColor;
            const VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                       .imageView = binding.colorView,
                                                       .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                       .loadOp = color->operations.loadOperation == AttachmentLoadOperation::Load
                                                                     ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                     : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                       .clearValue = {.color = {{clear.red, clear.green, clear.blue, clear.alpha}}}};
            const VkRenderingInfo operation{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                            .renderArea = {.offset = {0, 0},
                                                           .extent = {binding.textureDescriptor.extent.width,
                                                                      binding.textureDescriptor.extent.height}},
                                            .layerCount = 1,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &attachment};
            native.beginRendering(command, &operation);
            native.endRendering(command);
        }
    }

    /** @brief Copies submission ownership into preallocated tickets; no graph/request views survive admission. */
    Result<void> VulkanGraphRuntime::Impl::Record(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins) {
        for (auto &submission : submissions) {
            auto reserved = commands.Begin(submission.value.queue);
            if (reserved.HasError())
                return Result<void>::Failure(std::move(reserved).ErrorValue());
            submission.ticket = reserved.Value();
            auto retained = commands.Retain(submission.ticket, pins);
            if (retained.HasError())
                return retained;
            if (pins.lease == nullptr) {
                pins.lease = request.lease;
                if (request.transfer != nullptr)
                    request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
            }
            auto buffer = commands.RecordingBuffer(submission.ticket);
            if (buffer.HasError())
                return Result<void>::Failure(std::move(buffer).ErrorValue());
            for (std::size_t pass = submission.passes.offset; pass < submission.passes.offset + submission.passes.count; ++pass) {
                Barriers(buffer.Value(), passes[pass].before);
                Encode(buffer.Value(), request.workloads[pass].workload);
                Barriers(buffer.Value(), passes[pass].after);
            }
            if (auto finished = commands.Finish(submission.ticket); finished.HasError())
                return finished;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
