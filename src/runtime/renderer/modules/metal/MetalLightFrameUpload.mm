#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    /** @copydoc MetalResourceRuntime::UpdateLightFrame */
    Result<void> MetalResourceRuntime::UpdateLightFrame(const NativeLightFrameUpdate &native) {
        if (impl_->device == nil)
            return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
        if (const auto valid = ValidateLightFrameUpdate(native.update); valid.HasError())
            return valid;
        const auto &update = native.update;
        std::array<MetalBufferInstance *, 4> buffers{};
        const std::array<std::size_t, 4> requiredBytes{std::size_t{update.budget.maximumLights} * sizeof(PackedRenderLight),
                                                       std::size_t{update.budget.maximumClusters} * sizeof(PackedLightCluster),
                                                       std::size_t{update.budget.maximumClusters} * sizeof(PackedLightMembership),
                                                       std::size_t{update.budget.maximumClusters} * update.budget.referencesPerCluster *
                                                           sizeof(std::uint32_t)};
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            buffers[index] = Decode<MetalBufferInstance>(native.instances[index]);
            const auto *buffer = buffers[index];
            if (buffer == nullptr || !impl_->buffers.contains(buffers[index]) || buffer->use.retired ||
                !HasBufferUsage(buffer->usage, RenderBufferUsage::Storage) || buffer->buffer.storageMode != MTLStorageModeShared ||
                buffer->buffer.contents == nullptr || buffer->buffer.length < requiredBytes[index] ||
                update.revision <= buffer->lightTableRevision)
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            for (std::size_t previous = 0; previous < index; ++previous)
                if (buffers[previous] == buffer)
                    return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            if (!Impl::Complete(buffer->use.last) || !Impl::Complete(buffer->use.previous))
                return Result<void>::Failure(MakeError(LightCullingErrors::Pending));
            if (buffer->use.last.status == MTLCommandBufferStatusError || buffer->use.previous.status == MTLCommandBufferStatusError)
                return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed, "Prior light-slot native use failed."));
        }
        if (update.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
        // No failure remains after admission; partial validation never overwrites a GPU-visible table.
        if (!update.lights.empty())
            std::memcpy(buffers[0]->buffer.contents, update.lights.data(), update.lights.size_bytes());
        std::memcpy(buffers[1]->buffer.contents, update.clusters.data(), update.clusters.size_bytes());
        const LightCullingDispatch dispatch{static_cast<std::uint32_t>(update.lights.size()),
                                            static_cast<std::uint32_t>(update.clusters.size()), update.budget.referencesPerCluster, 0};
        for (auto *buffer : buffers) {
            buffer->lightTableRevision = update.revision;
            buffer->lightTableDispatch = dispatch;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
