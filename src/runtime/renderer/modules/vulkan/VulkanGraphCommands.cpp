#include "RenderMultiviewAttachmentContents.h"
#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>
#include <type_traits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Updates only admitted multiview attachment contents in the existing transactional scratch journal. */
        void PrepareMultiviewContents(const RenderGraphMultiview &workload, const std::span<const VulkanGraphResource> bindings,
                                      const std::span<VulkanGraphResourceState> states) {
            using enum MultiviewAttachmentAspect;
            // PreparePass already admitted the exact native targets. Keep this evidence in the same scratch
            // journal as ordinary attachments: neither recording nor an aborted/failed submission publishes it.
            for (const auto &target : workload.targets) {
                for (const auto aspect : {Color, Depth}) {
                    const auto resource = aspect == Color ? target.color : target.depth;
                    if (!resource.IsValid())
                        continue;
                    const auto index = resource.value - 1;
                    states[index].contentsDefined = MultiviewAttachmentContentsDefined(states[index].contentsDefined, workload, resource,
                                                                                       bindings[index].textureDescriptor, aspect);
                }
            }
        }
    }  // namespace

#include "VulkanGraphDepthAttachment.inl"

    /** @copydoc VulkanGraphRuntime::Impl::ValidateBufferCopy */
    Result<void> VulkanGraphRuntime::Impl::ValidateBufferCopy(const RenderGraphBufferCopy &copy) {
        const auto &source = bindings[copy.source.value - 1];
        const auto &destination = bindings[copy.destination.value - 1];
        if (const auto fits =
                [](const std::size_t offset, const std::size_t count, const std::size_t size) {
            return offset <= size && count <= size - offset;
        };
            source.kind != RenderGraphResourceKind::Buffer || destination.kind != RenderGraphResourceKind::Buffer ||
            source.buffer == VK_NULL_HANDLE || destination.buffer == VK_NULL_HANDLE || source.buffer == destination.buffer ||
            copy.byteCount == 0 || !HasBufferUsage(source.bufferDescriptor.usage, RenderBufferUsage::CopySource) ||
            !HasBufferUsage(destination.bufferDescriptor.usage, RenderBufferUsage::CopyDestination) ||
            !fits(copy.sourceOffset, copy.byteCount, source.bufferDescriptor.byteSize) ||
            !fits(copy.destinationOffset, copy.byteCount, destination.bufferDescriptor.byteSize))
            return Result<void>::Failure(Invalid("Buffer copy differs from native buffer identity, usage or checked byte bounds."));
        if (!states[copy.source.value - 1].contentsDefined)
            return Result<void>::Failure(Invalid("Buffer copy source contents are not defined."));
        if (copy.destinationOffset == 0 && copy.byteCount == destination.bufferDescriptor.byteSize)
            states[copy.destination.value - 1].contentsDefined = true;
        return Result<void>::Success();
    }

    /** @brief Validates native descriptor roles and contents before recording; unsupported adjacent capabilities remain explicit. */
    Result<void> VulkanGraphRuntime::Impl::ValidateWorkload(const RenderGraphWorkload &workload, const RenderQueueId queue) {
        if (const auto *multiviewWork = std::get_if<RenderGraphMultiview>(&workload)) {
            PrepareMultiviewContents(*multiviewWork, bindings, states);
            return Result<void>::Success();
        }
        if (std::holds_alternative<RenderGraphVfx>(workload))
            return Result<void>::Success();  // Complete native scene admission is performed with the full request in PreparePass.
        if (const auto *depth = std::get_if<RenderGraphDepthAttachment>(&workload))
            return ValidateDepthAttachment(*depth, queue);
        if (std::holds_alternative<std::monostate>(workload))
            return Result<void>::Success();
        if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload))
            return ValidateBufferCopy(*copy);
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            const auto &binding = bindings[color->texture.value - 1];
            if (const auto *nativeQueue = Queue(queue); nativeQueue == nullptr || !nativeQueue->supportsGraphics ||
                                                        binding.colorView == VK_NULL_HANDLE ||
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

    /** @copydoc VulkanGraphRuntime::Impl::Dispatch::EncodeCopy */
    void VulkanGraphRuntime::Impl::Dispatch::EncodeCopy(const VkCommandBuffer command, const RenderGraphBufferCopy &operation,
                                                        const std::span<const VulkanGraphResource> resident) const noexcept {
        const VkBufferCopy2 region{.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                                   .srcOffset = operation.sourceOffset,
                                   .dstOffset = operation.destinationOffset,
                                   .size = operation.byteCount};
        const VkCopyBufferInfo2 commandInfo{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                                            .srcBuffer = resident[operation.source.value - 1].buffer,
                                            .dstBuffer = resident[operation.destination.value - 1].buffer,
                                            .regionCount = 1,
                                            .pRegions = &region};
        copy(command, &commandInfo);
    }

    /** @copydoc VulkanGraphRuntime::Impl::Dispatch::EncodeColor */
    void VulkanGraphRuntime::Impl::Dispatch::EncodeColor(const VkCommandBuffer command, const RenderGraphColorAttachment &operation,
                                                         const std::span<const VulkanGraphResource> resident) const noexcept {
        const auto &binding = resident[operation.texture.value - 1];
        const auto &clear = operation.operations.clearColor;
        const VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                   .imageView = binding.colorView,
                                                   .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                   .loadOp = operation.operations.loadOperation == AttachmentLoadOperation::Load
                                                                 ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                 : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                   .clearValue = {.color = {{clear.red, clear.green, clear.blue, clear.alpha}}}};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {.offset = {0, 0},
                                                       .extent = {binding.textureDescriptor.extent.width,
                                                                  binding.textureDescriptor.extent.height}},
                                        .layerCount = binding.textureDescriptor.layerCount,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &attachment};
        beginRendering(command, &rendering);
        endRendering(command);
    }

    /** @brief Encodes only a workload already proven against exact resident descriptors. */
    void VulkanGraphRuntime::Impl::Encode(const VkCommandBuffer command, const RenderGraphWorkload &workload) const noexcept {
        std::visit([this, command]<typename Operation>(const Operation &operation) noexcept {
            if constexpr (std::is_same_v<Operation, RenderGraphMultiview>) {
                multiview.Encode(command);
            } else if constexpr (std::is_same_v<Operation, RenderGraphDepthAttachment>) {
                native.EncodeDepthAttachment(command, operation, bindings);
            } else if constexpr (std::is_same_v<Operation, RenderGraphBufferCopy>) {
                native.EncodeCopy(command, operation, bindings);
            } else if constexpr (std::is_same_v<Operation, RenderGraphColorAttachment>) {
                native.EncodeColor(command, operation, bindings);
            } else if constexpr (!std::is_same_v<Operation, std::monostate>) {
                HORO_INVARIANT_MSG(false, "Unsupported native workload passed validation.");
            }
        }, workload);
    }

    /** @brief Copies submission ownership into preallocated tickets; no graph/request views survive admission. */
    Result<void> VulkanGraphRuntime::Impl::Record(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins) {
        for (auto &submission : submissions) {
            auto reserved = commands.Begin(submission.value.queue);
            if (reserved.HasError())
                return Result<void>::Failure(std::move(reserved).ErrorValue());
            submission.ticket = reserved.Value();
            if (auto retained = commands.Retain(submission.ticket, pins); retained.HasError())
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
                if (std::holds_alternative<RenderGraphVfx>(request.workloads[pass].workload))
                    EncodeVfx(buffer.Value(), pass);
                else
                    Encode(buffer.Value(), request.workloads[pass].workload);
                Barriers(buffer.Value(), passes[pass].after);
            }
            if (auto finished = commands.Finish(submission.ticket); finished.HasError())
                return finished;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
