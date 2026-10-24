#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        bool Complete(id<MTLCommandBuffer> commands) noexcept {
            return !commands || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
        }

        /** @brief Proves exact mapped extent and completed use before any member of a UI slot is modified. */
        Result<void> Writable(const MetalBufferInstance &buffer, RenderBufferUsage usage, std::size_t bytes, std::uint64_t revision) {
            if (buffer.use.retired || !HasBufferUsage(buffer.usage, usage) || buffer.buffer.storageMode != MTLStorageModeShared ||
                !buffer.buffer.contents || bytes > buffer.buffer.length || revision <= buffer.uiPaintRevision)
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            if (!Complete(buffer.use.last) || !Complete(buffer.use.previous))
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::Pending, "UI slot is still used by native commands."));
            if (buffer.use.last.status == MTLCommandBufferStatusError || buffer.use.previous.status == MTLCommandBufferStatusError)
                return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::UpdateUiPaintFrame */
    Result<void> MetalResourceRuntime::UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &native) {
        if (!impl_->device)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (const auto valid = ValidateUiPaintFrameUpdate(native.update); valid.HasError())
            return valid;
        const auto &update = native.update;
        const std::array<std::size_t, 3> bytes{update.vertices.size_bytes(), update.indices.size_bytes(),
                                               update.parameters.size() * UiPaintFrameUpdate::ParameterStride};
        constexpr std::array usage{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
        std::array<MetalBufferInstance *, 3> buffers{};
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            auto *buffer = Decode<MetalBufferInstance>(native.instances[index]);
            if (!buffer || !impl_->buffers.contains(buffer))
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            if (const auto writable = Writable(*buffer, usage[index], bytes[index], update.revision); writable.HasError())
                return writable;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (buffers[previous] == buffer)
                    return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            buffers[index] = buffer;
        }
        if (update.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
        // All three destinations are now admitted; mapped copies cannot produce a partially published slot.
        std::memcpy(buffers[0]->buffer.contents, update.vertices.data(), update.vertices.size_bytes());
        std::memcpy(buffers[1]->buffer.contents, update.indices.data(), update.indices.size_bytes());
        auto *parameters = static_cast<std::byte *>(buffers[2]->buffer.contents);
        for (std::size_t index = 0; index < update.parameters.size(); ++index)
            std::memcpy(parameters + index * UiPaintFrameUpdate::ParameterStride, &update.parameters[index], sizeof(UiPaintParameters));
        for (auto *buffer : buffers) {
            buffer->uiPaintRevision = update.revision;
            buffer->uiPaintVertices = static_cast<std::uint32_t>(update.vertices.size());
            buffer->uiPaintIndices = static_cast<std::uint32_t>(update.indices.size());
            buffer->uiPaintParameters = static_cast<std::uint32_t>(update.parameters.size());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
