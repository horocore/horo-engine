#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Requires exactly one native instance-uniform binding with the reflected block extent and full-buffer range. */
        Result<void> ValidateInstanceUploadBinding(const NativeVfxFrameUpdate &update, const MetalBufferInstance &buffer,
                                                   const MetalGraphicsPipeline &pipeline, const MetalMaterialBinding &material) {
            const auto mapping = std::ranges::find_if(update.layout->targetBindings, [](const auto &entry) {
                return entry.id == VfxShaderParameterIds::Instances && entry.active && entry.generatedHelperIndex == 0;
            });
            if (mapping == update.layout->targetBindings.end() || mapping->nativeBinding >= pipeline.bufferSizes.size() ||
                pipeline.bufferSizes[mapping->nativeBinding] != update.parameters.size())
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            std::size_t matches{};
            for (const auto &slot : std::span{material.slots}.first(material.count))
                if (slot.kind == MaterialResidentResourceKind::Buffer && slot.buffer == buffer.buffer) {
                    if (slot.binding != mapping->nativeBinding || slot.offset != 0 || slot.byteCount != update.parameters.size())
                        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                    ++matches;
                }
            if (matches != 1)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::UpdateVfxFrame */
    Result<void> MetalResourceRuntime::UpdateVfxFrame(const NativeVfxFrameUpdate &update) {
        if (impl_->device == nil || update.deviceIncarnation == 0 || update.deviceIncarnation != impl_->deviceIncarnation ||
            !update.layout || !update.writeStatus || *update.writeStatus != VfxFrameWriteStatus::NotStarted || update.revision == 0 ||
            update.instanceCount == 0 || update.instanceCount > MaximumVfxDrawInstances)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        auto *buffer = Decode<MetalBufferInstance>(update.instance);
        auto *pipeline = Decode<MetalGraphicsPipeline>(update.pipeline);
        const auto *material = dynamic_cast<const MetalMaterialBinding *>(update.material);
        if (!impl_->buffers.contains(buffer) || !impl_->graphicsPipelines.contains(pipeline) || !material ||
            material->owner != impl_.get() || material->deviceIncarnation != update.deviceIncarnation ||
            material->count > material->slots.size() || material->sourcePipeline != pipeline || material->pipeline != pipeline->pipeline ||
            !SameMetalMaterialLayout(*update.layout, pipeline->layout) || VfxShaderParameterBytes(*update.layout).HasError() ||
            buffer->use.retired || buffer->deviceIncarnation != update.deviceIncarnation ||
            buffer->descriptor.usage != RenderBufferUsage::Uniform || buffer->descriptor.access != RenderBufferAccess::HostVisible ||
            buffer->buffer.storageMode != MTLStorageModeShared || update.parameters.size() != buffer->descriptor.byteSize ||
            update.parameters.size() > buffer->buffer.length || update.revision <= buffer->vfxRevision)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (const auto binding = ValidateInstanceUploadBinding(update, *buffer, *pipeline, *material); binding.HasError())
            return binding;
        // These are the existing canonical command uses, including non-VFX aliases and older submitted work after abort.
        if (!Impl::Complete(buffer->use.last) || !Impl::Complete(buffer->use.previous))
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        if (buffer->use.last.status == MTLCommandBufferStatusError || buffer->use.previous.status == MTLCommandBufferStatusError)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        if (update.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
        auto *destination = buffer->buffer.contents;
        if (!destination)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        *update.writeStatus = VfxFrameWriteStatus::Attempted;
        std::memcpy(destination, update.parameters.data(), update.parameters.size());
        buffer->vfxRevision = update.revision;
        buffer->vfxInstances = update.instanceCount;
        buffer->vfxPublished = true;
        *update.writeStatus = VfxFrameWriteStatus::Committed;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
