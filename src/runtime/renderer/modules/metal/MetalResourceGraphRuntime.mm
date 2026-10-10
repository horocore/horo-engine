#include "MetalColorAttachmentEncoding.h"
#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Finds only an exact graph-local resource binding; never guesses by slot or native address. */
        [[nodiscard]] std::uint64_t GraphInstance(const RenderGraphResourceId id,
                                                  const std::span<const RenderGraphResourceInstance> resources) {
            for (const auto &resource : resources) {
                if (resource.resource == id) {
                    return resource.instance;
                }
            }
            return 0;
        }

        /** @brief Checks class membership before dereferencing a graph-local native identity. */
        template <typename Instance>
        [[nodiscard]] Instance *LiveGraphInstance(const RenderGraphResourceId id,
                                                  const std::span<const RenderGraphResourceInstance> resources,
                                                  const std::unordered_set<Instance *> &residents) {
            Instance *instance = Decode<Instance>(GraphInstance(id, resources));
            return instance != nullptr && residents.contains(instance) && !instance->use.retired ? instance : nullptr;
        }

        [[nodiscard]] bool ValidColorOperations(const RenderGraphColorAttachment &color, const RenderTextureDescriptor &descriptor) {
            return color.operations.IsValid() && HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) &&
                   descriptor.mipCount == 1 && descriptor.layerCount == 1 && descriptor.sampleCount == 1 &&
                   MetalTextureAspectMatches(descriptor.format, RenderTextureAspect::Color);
        }

        [[nodiscard]] Result<void> ValidateGraphColor(const RenderGraphColorAttachment &color,
                                                      const std::span<const RenderGraphResourceInstance> resources,
                                                      const std::unordered_set<MetalTextureInstance *> &textures) {
            const auto *texture = LiveGraphInstance(color.texture, resources, textures);
            if (texture == nullptr)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            if (!ValidColorOperations(color, texture->descriptor))
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                       "Metal graph color operations require a single color mip, layer and sample."));
            return Result<void>::Success();
        }

        [[nodiscard]] bool ValidCopyRanges(const RenderGraphBufferCopy &copy, const MetalBufferInstance &source,
                                           const MetalBufferInstance &destination) {
            return HasBufferUsage(source.usage, RenderBufferUsage::CopySource) &&
                   HasBufferUsage(destination.usage, RenderBufferUsage::CopyDestination) && &source != &destination &&
                   copy.byteCount != 0 && copy.sourceOffset <= source.buffer.length &&
                   copy.byteCount <= source.buffer.length - copy.sourceOffset && copy.destinationOffset <= destination.buffer.length &&
                   copy.byteCount <= destination.buffer.length - copy.destinationOffset;
        }

        [[nodiscard]] Result<void> ValidateGraphCopy(const RenderGraphBufferCopy &copy,
                                                     const std::span<const RenderGraphResourceInstance> resources,
                                                     const std::unordered_set<MetalBufferInstance *> &buffers) {
            const auto *source = LiveGraphInstance(copy.source, resources, buffers);
            const auto *destination = LiveGraphInstance(copy.destination, resources, buffers);
            if (source == nullptr || destination == nullptr)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            if (!ValidCopyRanges(copy, *source, *destination))
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan,
                                                       "Metal graph copy byte range exceeds its resolved resident buffer."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateGraphWorkload */
    Result<void> MetalResourceRuntime::ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                             const std::span<const RenderGraphResourceInstance> resources) const {
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload))
            return ValidateGraphColor(*color, resources, impl_->textures);
        if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload))
            return ValidateGraphCopy(*copy, resources, impl_->buffers);
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::ExecuteGraphWorkload */
    Result<void> MetalResourceRuntime::ExecuteGraphWorkload(void *commandBuffer, const RenderGraphWorkload &workload,
                                                            const std::span<const RenderGraphResourceInstance> resources) {
        if (const auto valid = ValidateGraphWorkload(workload, resources); valid.HasError()) {
            return valid;
        }
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)commandBuffer;
        if (commands == nil) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        }
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            auto *texture = Decode<MetalTextureInstance>(GraphInstance(color->texture, resources));
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            auto *attachment = pass.colorAttachments[0];
            attachment.texture = texture->texture;
            ConfigureMetalColorAttachment(attachment, color->operations);
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil) {
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::CommandSubmissionFailed, "Metal graph color encoder creation failed."));
            }
            [encoder endEncoding];
            Impl::TrackUse(texture->use, commands);
        } else if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            auto *source = Decode<MetalBufferInstance>(GraphInstance(copy->source, resources));
            auto *destination = Decode<MetalBufferInstance>(GraphInstance(copy->destination, resources));
            id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
            if (encoder == nil) {
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::CommandSubmissionFailed, "Metal graph copy encoder creation failed."));
            }
            [encoder copyFromBuffer:source->buffer
                       sourceOffset:copy->sourceOffset
                           toBuffer:destination->buffer
                  destinationOffset:copy->destinationOffset
                               size:copy->byteCount];
            [encoder endEncoding];
            Impl::TrackUse(source->use, commands);
            Impl::TrackUse(destination->use, commands);
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::CaptureGraphOperation */
    Result<MetalRecordedOperation> MetalResourceRuntime::CaptureGraphOperation(const RenderGraphWorkload &workload,
                                                                               const std::span<const RenderGraphResourceInstance> resources,
                                                                               id<MTLCommandBuffer> commands, id<MTLTexture> primary) {
        if (const auto valid = ValidateGraphWorkload(workload, resources); valid.HasError())
            return Result<MetalRecordedOperation>::Failure(valid.ErrorValue());
        MetalRecordedOperation operation{.workload = workload, .commands = commands};
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            auto *texture = Decode<MetalTextureInstance>(GraphInstance(color->texture, resources));
            operation.texture = texture->texture;
            operation.sourceHeap = texture->texture.heap;
        } else if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            auto *source = Decode<MetalBufferInstance>(GraphInstance(copy->source, resources));
            auto *destination = Decode<MetalBufferInstance>(GraphInstance(copy->destination, resources));
            operation.source = source->buffer;
            operation.destination = destination->buffer;
            operation.sourceHeap = source->buffer.heap;
            operation.destinationHeap = destination->buffer.heap;
        } else if (std::holds_alternative<PrimaryOutputAttachment>(workload)) {
            operation.texture = primary;
        }
        return Result<MetalRecordedOperation>::Success(std::move(operation));
    }

    /** @copydoc MetalResourceRuntime::TrackParallelGraphUse */
    void MetalResourceRuntime::TrackParallelGraphUse(void *finalCommands,
                                                     const std::span<const RenderGraphResourceInstance> resources) noexcept {
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)finalCommands;
        for (const auto &resource : resources) {
            auto *buffer = Decode<MetalBufferInstance>(resource.instance);
            if (impl_->buffers.contains(buffer))
                Impl::TrackUse(buffer->use, commands);
            auto *texture = Decode<MetalTextureInstance>(resource.instance);
            if (impl_->textures.contains(texture))
                Impl::TrackUse(texture->use, commands);
        }
    }

    /** @copydoc MetalResourceRuntime::FinishGraphCommands */
    void MetalResourceRuntime::FinishGraphCommands(void *commandBuffer, const bool committed) noexcept {
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)commandBuffer;
        for (auto *buffer : impl_->buffers) {
            Impl::FinishUse(buffer->use, commands, committed);
        }
        for (auto *texture : impl_->textures) {
            Impl::FinishUse(texture->use, commands, committed);
        }
        impl_->DrainRetired(64);
    }

    /** @copydoc MetalResourceRuntime::DrainGraphRetirements */
    void MetalResourceRuntime::DrainGraphRetirements() noexcept {
        impl_->DrainRetired(64);
    }

}  // namespace Horo::Render::Detail
