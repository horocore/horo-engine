#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error MakeFrontendError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    Result<void> RenderFrontend::ValidateMeshDependencies(const RenderMeshDescriptor &descriptor) const {
        const auto vertex = resourceRegistry_->State(Detail::RenderResourceClass::Buffer, Identity(descriptor.vertexBuffer));
        if (vertex.HasError()) {
            return Result<void>::Failure(vertex.ErrorValue());
        }
        const auto index = resourceRegistry_->State(Detail::RenderResourceClass::Buffer, Identity(descriptor.indexBuffer));
        if (index.HasError()) {
            return Result<void>::Failure(index.ErrorValue());
        }
        if (vertex.Value() != RenderResourceState::Ready || index.Value() != RenderResourceState::Ready) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::ResourceDependencyNotReady, "Mesh creation requires ready vertex and index buffers."));
        }
        return Result<void>::Success();
    }

    bool RenderFrontend::IsMeshBufferLayoutCompatible(const RenderMeshDescriptor &descriptor) const noexcept {
        if (descriptor.vertexBuffer.slot >= buffers_.size() || descriptor.indexBuffer.slot >= buffers_.size()) {
            return false;
        }
        const BufferRecord &vertex = buffers_[descriptor.vertexBuffer.slot];
        const BufferRecord &index = buffers_[descriptor.indexBuffer.slot];
        const std::uint32_t indexSize = descriptor.indexFormat == RenderIndexFormat::UInt16 ? 2U : 4U;
        const bool recordsMatch =
            vertex.generation == descriptor.vertexBuffer.generation && index.generation == descriptor.indexBuffer.generation;
        return recordsMatch && HasBufferUsage(vertex.descriptor.usage, RenderBufferUsage::Vertex) &&
               HasBufferUsage(index.descriptor.usage, RenderBufferUsage::Index) &&
               FitsBuffer(descriptor.vertexStride, descriptor.vertexCount, vertex.descriptor.byteSize) &&
               FitsBuffer(indexSize, descriptor.indexCount, index.descriptor.byteSize);
    }

    Result<void> RenderFrontend::ValidateTextureViewDependency(const RenderTextureViewDescriptor &descriptor) const {
        const auto textureState = resourceRegistry_->State(Detail::RenderResourceClass::Texture, Identity(descriptor.texture));
        if (textureState.HasError())
            return Result<void>::Failure(textureState.ErrorValue());
        if (textureState.Value() != RenderResourceState::Ready) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::ResourceDependencyNotReady, "Texture-view creation requires a ready texture."));
        }
        if (descriptor.texture.slot >= textures_.size()) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::InvalidTextureViewDescriptor, "Texture-view source metadata is unavailable."));
        }
        if (const TextureRecord &texture = textures_[descriptor.texture.slot];
            texture.generation != descriptor.texture.generation ||
            ValidateRenderTextureViewCompatibility(texture.descriptor, descriptor).HasError()) {
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::InvalidTextureViewDescriptor,
                                                           "Texture-view format, range, or aspect is incompatible with its texture."));
        }
        return Result<void>::Success();
    }

    Result<void> RenderFrontend::ValidateRenderTargetDependencies(const RenderTargetDescriptor &descriptor) const {
        if (const Result<void> color = ValidateRenderTargetAttachment(descriptor.colorAttachment, RenderTextureAspect::Color,
                                                                      descriptor.extent, descriptor.sampleCount);
            color.HasError())
            return color;
        if (const Result<void> depth = ValidateRenderTargetAttachment(descriptor.depthAttachment, RenderTextureAspect::Depth,
                                                                      descriptor.extent, descriptor.sampleCount);
            depth.HasError())
            return depth;
        return Result<void>::Success();
    }

    Result<void> RenderFrontend::ValidateRenderTargetAttachment(const RenderTextureViewHandle handle,
                                                                const RenderTextureAspect requiredAspect, const FramebufferExtent extent,
                                                                const std::uint32_t sampleCount) const {
        if (!handle.IsValid())
            return Result<void>::Success();
        const auto state = resourceRegistry_->State(Detail::RenderResourceClass::TextureView, Identity(handle));
        if (state.HasError())
            return Result<void>::Failure(state.ErrorValue());
        if (state.Value() != RenderResourceState::Ready)
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::ResourceDependencyNotReady, "Render-target creation requires ready attachment views."));
        if (handle.slot >= textureViews_.size())
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::InvalidRenderTargetDescriptor, "Render-target attachment metadata is unavailable."));
        const TextureViewRecord &view = textureViews_[handle.slot];
        if (const bool aspectCompatible = requiredAspect == RenderTextureAspect::Color
                                              ? view.descriptor.aspect == RenderTextureAspect::Color
                                              : view.descriptor.aspect != RenderTextureAspect::Color;
            view.generation != handle.generation || !aspectCompatible || view.descriptor.texture.slot >= textures_.size())
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::InvalidRenderTargetDescriptor, "Render-target attachment aspect is incompatible."));
        const TextureRecord &texture = textures_[view.descriptor.texture.slot];
        if (const std::array compatible{
                texture.descriptor.extent == extent,
                texture.descriptor.sampleCount == sampleCount,
                HasTextureUsage(texture.descriptor.usage, RenderTextureUsage::RenderAttachment),
            };
            !std::ranges::all_of(compatible, std::identity{}))
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::InvalidRenderTargetDescriptor,
                                                           "Render-target attachment extent, samples, or usage is incompatible."));
        return Result<void>::Success();
    }

    bool RenderFrontend::IsLiveTarget(const RenderTargetHandle target, const FramebufferExtent extent) const noexcept {
        const auto state =
            resourceRegistry_->State(Detail::RenderResourceClass::RenderTarget, {target.owner, target.slot, target.generation});
        return state.HasValue() && state.Value() == RenderResourceState::Ready && target.slot < targets_.size() &&
               targets_[target.slot].extent.width == extent.width && targets_[target.slot].extent.height == extent.height;
    }
}  // namespace Horo::Render
