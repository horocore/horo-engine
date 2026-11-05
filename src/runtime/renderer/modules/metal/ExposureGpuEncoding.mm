#include "ExposureGpuKernel.h"
#include "MetalResourceRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves only registered live native graph instances, rejecting stale raw tokens. */
        template <class T>
        T *Resolve(RenderGraphResourceId id, std::span<const RenderGraphResourceInstance> resources,
                   const std::unordered_set<T *> &live) noexcept {
            const auto found = std::ranges::find(resources, id, &RenderGraphResourceInstance::resource);
            if (found == resources.end())
                return nullptr;
            auto *instance = Decode<T>(found->instance);
            return live.contains(instance) ? instance : nullptr;
        }

        /** @brief Requires actual exact device-local storage bounds and the operation's fixed usage. */
        bool BufferValid(const MetalBufferInstance *buffer, std::size_t bytes, std::uint64_t incarnation) noexcept {
            return buffer && buffer->buffer != nil && buffer->deviceIncarnation == incarnation && buffer->buffer.length == bytes &&
                   buffer->descriptor.byteSize == bytes && buffer->descriptor.access == RenderBufferAccess::DeviceLocal &&
                   HasBufferUsage(buffer->usage, RenderBufferUsage::Storage) && !buffer->use.retired;
        }

        /** @brief Requires the actual canonical unexposed source storage shape. */
        bool TextureValid(const MetalTextureInstance *source, FramebufferExtent extent) noexcept {
            if (!source || source->texture == nil || source->use.retired)
                return false;
            const auto &d = source->descriptor;
            return d.format == RenderTextureFormat::Rgba16Float && d.extent == extent && d.dimension == RenderTextureDimension::TwoD &&
                   d.layerCount == 1 && d.mipCount == 1 && d.sampleCount == 1 && HasTextureUsage(d.usage, RenderTextureUsage::Sampled);
        }

        /** @brief Admits terminal/accepted same-queue use or ordering within the trusted actual current native command. */
        bool SameQueueUse(const MetalResidentUse &use, id<MTLCommandQueue> queue, id<MTLCommandBuffer> current) noexcept {
            if (use.last == nil)
                return true;
            if (current != nil && use.last == current && current.commandQueue == queue)
                return true;
            if (use.last.status == MTLCommandBufferStatusCompleted)
                return true;
            if (use.last.commandQueue != queue)
                return false;
            return use.last.status == MTLCommandBufferStatusCommitted || use.last.status == MTLCommandBufferStatusScheduled;
        }

        /** @brief Encodes one exact stage with its actual reflected native resource indices. */
        void EncodeStage(id<MTLComputeCommandEncoder> encoder, const ExposureGpuKernel &kernel, std::size_t stage, id<MTLTexture> source,
                         const std::array<MetalBufferInstance *, 3> &buffers, const RenderGraphExposureGpu &work) {
            [encoder setComputePipelineState:kernel.pipelines[stage]];
            const auto &bindings = kernel.bindings[stage];
            if (bindings[0] != NSNotFound)
                [encoder setTexture:source atIndex:bindings[0]];
            for (std::size_t i = 0; i < buffers.size(); ++i)
                if (bindings[i + 1] != NSNotFound)
                    [encoder setBuffer:buffers[i]->buffer offset:0 atIndex:bindings[i + 1]];
            auto parameters = work.dispatch;
            if (work.resetPrevious)
                parameters.exposure.controls[2] |= 0x80000000U;
            [encoder setBytes:&parameters length:sizeof(ExposureGpuDispatchParameters) atIndex:bindings[4]];
            if (stage == 1) {
                const auto &region = work.dispatch.exposure.region;
                const auto groupsX = region[2] / 8U + (region[2] % 8U != 0);
                const auto groupsY = region[3] / 8U + (region[3] % 8U != 0);
                [encoder dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            } else
                [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        }

        /** @brief Zeroes only an explicit fresh inactive seed on the GPU before compute. */
        Result<void> ResetSeed(id<MTLCommandBuffer> commands, id<MTLBuffer> previous) {
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            if (blit == nil)
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            [blit fillBuffer:previous range:NSMakeRange(0, sizeof(ExposureGpuState)) value:0];
            [blit endEncoding];
            return Result<void>::Success();
        }
    }  // namespace

    namespace {
        /** @brief Retains no ownership beyond synchronous encoding; registry leases own the actual instances. */
        struct ExposureResources final {
            MetalTextureInstance *source{};
            std::array<MetalBufferInstance *, 3> buffers{};
        };

        /** @brief Rejects distinct graph tokens that alias the same native buffer. */
        bool BuffersDistinct(const ExposureResources &r) noexcept {
            for (std::size_t i = 0; i < r.buffers.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (r.buffers[i]->buffer == r.buffers[j]->buffer)
                        return false;
            return true;
        }

        /** @brief Resolves exact fixed native resources and checks their device-local bounds. */
        template <class Impl>
        bool ResolveResources(const Impl &impl, const RenderGraphExposureGpu &w, std::span<const RenderGraphResourceInstance> resources,
                              ExposureResources &r, id<MTLCommandBuffer> current) noexcept {
            r.source = Resolve(w.sceneColor, resources, impl.textures);
            if (!TextureValid(r.source, w.sourceExtent))
                return false;
            if (!SameQueueUse(r.source->use, impl.commandQueue, current))
                return false;
            const std::array ids{w.histogram, w.previousExposure, w.outputExposure};
            const std::array<std::size_t, 3> sizes{ExposureHistogramBins * sizeof(std::uint32_t), sizeof(ExposureGpuState),
                                                   sizeof(ExposureGpuState)};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                r.buffers[i] = Resolve(ids[i], resources, impl.buffers);
                if (!BufferValid(r.buffers[i], sizes[i], impl.deviceIncarnation))
                    return false;
                if (!SameQueueUse(r.buffers[i]->use, impl.commandQueue, current))
                    return false;
            }
            return BuffersDistinct(r);
        }

        /** @brief Checks the explicit reset seed is inactive and admits a real GPU fill. */
        template <class Impl> bool ResetValid(const RenderGraphExposureGpu &w, const ExposureResources &r) noexcept {
            if (!w.resetPrevious)
                return true;
            return HasBufferUsage(r.buffers[1]->usage, RenderBufferUsage::CopyDestination) && Impl::Complete(r.buffers[1]->use.last);
        }

        /** @brief Rejects buffers owned by another live family journal, including retained last-good states. */
        template <class Impl>
        bool ForeignBuffersValid(const Impl &impl, const ExposureGpuKernel *kernel, const RenderGraphExposureGpu &w,
                                 const ExposureResources &r) noexcept {
            for (const auto &weak : impl.exposureGpuKernels) {
                const auto other = weak.lock();
                if (!other || other.get() == kernel)
                    continue;
                for (const auto &entry : other->journal)
                    if (entry.buffer == r.buffers[2]->buffer || (w.resetPrevious && entry.buffer == r.buffers[1]->buffer))
                        return false;
            }
            return true;
        }

        /** @brief Checks runtime-issued family identity independently of public descriptor metadata. */
        template <class Impl>
        bool KernelValid(const Impl &impl, const ExposureGpuKernel *kernel, const RenderGraphExposureGpu &w) noexcept {
            return kernel && kernel->lane == std::this_thread::get_id() && kernel->owner == impl.exposureGpuOwner &&
                   kernel->incarnation == impl.deviceIncarnation && w.produced.deviceGeneration == impl.deviceIncarnation;
        }

        /** @brief Performs only the fixed native fill/clear/histogram/adaptation sequence. */
        Result<void> EncodeFamily(id<MTLCommandBuffer> commands, const ExposureGpuKernel &kernel, const ExposureResources &r,
                                  const RenderGraphExposureGpu &w) {
            if (w.resetPrevious)
                if (auto reset = ResetSeed(commands, r.buffers[1]->buffer); reset.HasError())
                    return reset;
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            if (encoder == nil)
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            for (std::size_t stage = 0; stage < 3; ++stage) {
                EncodeStage(encoder, kernel, stage, r.source->texture, r.buffers, w);
                if (stage != 2)
                    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            }
            [encoder endEncoding];
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateExposureGpu */
    Result<void> MetalResourceRuntime::ValidateExposureGpu(const RenderGraphExposureGpu &work,
                                                           std::span<const RenderGraphResourceInstance> resources,
                                                           void *currentCommands) const {
        if (auto portable = ValidateExposureGpuWorkload(work); portable.HasError())
            return portable;
        auto *kernel = dynamic_cast<ExposureGpuKernel *>(work.kernel.get());
        if (!KernelValid(*impl_, kernel, work))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        id<MTLCommandBuffer> current = (__bridge id<MTLCommandBuffer>)currentCommands;
        if (current != nil && current.commandQueue != impl_->commandQueue)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        ExposureResources resolved;
        if (!ResolveResources(*impl_, work, resources, resolved, current))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!ResetValid<Impl>(work, resolved) || !ForeignBuffersValid(*impl_, kernel, work, resolved))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        auto journal = kernel->Prepare(work, resolved.buffers[1]->buffer, resolved.buffers[2]->buffer, impl_->commandQueue);
        return journal.HasError() ? Result<void>::Failure(journal.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::ExecuteExposureGpu */
    Result<void> MetalResourceRuntime::ExecuteExposureGpu(void *commandBuffer, const RenderGraphExposureGpu &work,
                                                          std::span<const RenderGraphResourceInstance> resources) {
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)commandBuffer;
        if (commands == nil || commands.commandQueue != impl_->commandQueue)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (auto valid = ValidateExposureGpu(work, resources, commandBuffer); valid.HasError())
            return valid;
        auto &kernel = *dynamic_cast<ExposureGpuKernel *>(work.kernel.get());
        ExposureResources resolved;
        if (!ResolveResources(*impl_, work, resources, resolved, commands))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        auto slot = kernel.Prepare(work, resolved.buffers[1]->buffer, resolved.buffers[2]->buffer, commands.commandQueue);
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        if (auto encoded = EncodeFamily(commands, kernel, resolved, work); encoded.HasError())
            return encoded;
        for (auto *buffer : resolved.buffers)
            Impl::TrackUse(buffer->use, commands);
        Impl::TrackUse(resolved.source->use, commands);
        kernel.Record(slot.Value(), work, resolved.buffers[2]->buffer, commands);
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
