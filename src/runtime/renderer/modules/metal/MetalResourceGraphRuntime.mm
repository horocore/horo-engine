#include "MetalColorAttachmentEncoding.h"
#include "MetalLightCullingKernel.h"
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

        /** @brief Validates native kernel provenance and finite whole-buffer ranges before encoding. */
        [[nodiscard]] Result<void> ValidateGraphLightCulling(const RenderGraphLightCulling &culling,
                                                             const std::span<const RenderGraphResourceInstance> resources,
                                                             const std::unordered_set<MetalBufferInstance *> &buffers, const void *owner,
                                                             const std::uint64_t incarnation, const bool exhausted) {
            const auto *kernel = dynamic_cast<const MetalLightCullingKernel *>(culling.kernel.get());
            if (kernel == nullptr || kernel->pipeline == nil || kernel->owner.get() != owner || kernel->incarnation != incarnation ||
                exhausted)
                return Result<void>::Failure(
                    MakeError(LightCullingErrors::InvalidInput, "Native light kernel lease belongs to another runtime incarnation."));
            const auto &dispatch = culling.dispatch;
            const LightCullingBudget bounds{.maximumLights = std::max(dispatch.referencesPerCluster, std::max(1U, dispatch.lightCount)),
                                            .maximumClusters = dispatch.clusterCount,
                                            .referencesPerCluster = dispatch.referencesPerCluster};
            if (!bounds.IsValid() || dispatch.reserved != 0)
                return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
            const std::array ids{culling.lights, culling.clusters, culling.membership, culling.references};
            const std::array<std::size_t, 4> bytes{std::max<std::size_t>(1, dispatch.lightCount) * sizeof(PackedRenderLight),
                                                   dispatch.clusterCount * sizeof(PackedLightCluster),
                                                   dispatch.clusterCount * sizeof(PackedLightMembership),
                                                   std::size_t{dispatch.clusterCount} * dispatch.referencesPerCluster *
                                                       sizeof(std::uint32_t)};
            std::array<const MetalBufferInstance *, 4> resolved{};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                resolved[index] = LiveGraphInstance(ids[index], resources, buffers);
                if (resolved[index] == nullptr || !HasBufferUsage(resolved[index]->usage, RenderBufferUsage::Storage) ||
                    bytes[index] > resolved[index]->buffer.length || culling.tableRevision == 0 ||
                    resolved[index]->lightTableRevision != culling.tableRevision ||
                    resolved[index]->lightTableDispatch.lightCount != dispatch.lightCount ||
                    resolved[index]->lightTableDispatch.clusterCount != dispatch.clusterCount ||
                    resolved[index]->lightTableDispatch.referencesPerCluster != dispatch.referencesPerCluster)
                    return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (resolved[previous] == resolved[index])
                        return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            }
            return Result<void>::Success();
        }

        /** @brief Encodes an already validated light workload; the caller pins its buffers after success. */
        Result<void> EncodeGraphLightCulling(id<MTLCommandBuffer> commands, const RenderGraphLightCulling &culling,
                                             const std::array<MetalBufferInstance *, 4> &buffers) {
            const auto &kernel = *dynamic_cast<const MetalLightCullingKernel *>(culling.kernel.get());
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            if (encoder == nil)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::CommandSubmissionFailed, "Metal light compute encoder creation failed."));
            [encoder setComputePipelineState:kernel.pipeline];
            for (std::size_t index = 0; index < buffers.size(); ++index)
                [encoder setBuffer:buffers[index]->buffer offset:0 atIndex:kernel.bindings[index]];
            [encoder setBytes:&culling.dispatch length:sizeof(LightCullingDispatch) atIndex:kernel.bindings[4]];
            [encoder dispatchThreads:MTLSizeMake(culling.dispatch.clusterCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding];
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateGraphWorkload */
    Result<void> MetalResourceRuntime::ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                             const std::span<const RenderGraphResourceInstance> resources) const {
        if (const auto *culling = std::get_if<RenderGraphLightCulling>(&workload))
            return ValidateGraphLightCulling(*culling, resources, impl_->buffers, impl_->lightKernelOwner.get(),
                                             impl_->lightKernelIncarnation, impl_->lightKernelIdentityExhausted);
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
        if (const auto *culling = std::get_if<RenderGraphLightCulling>(&workload)) {
            const std::array ids{culling->lights, culling->clusters, culling->membership, culling->references};
            std::array<MetalBufferInstance *, 4> buffers{};
            for (std::size_t index = 0; index < ids.size(); ++index)
                buffers[index] = Decode<MetalBufferInstance>(GraphInstance(ids[index], resources));
            if (const auto encoded = EncodeGraphLightCulling(commands, *culling, buffers); encoded.HasError())
                return encoded;
            for (auto *buffer : buffers)
                Impl::TrackUse(buffer->use, commands);
        } else if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
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
        if (std::holds_alternative<RenderGraphLightCulling>(workload))
            return Result<MetalRecordedOperation>::Failure(
                MakeError(LightCullingErrors::Unsupported, "Light compute currently requires owner-thread graph encoding."));
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
