#include "MetalColorAttachmentEncoding.h"
#include "MetalMaterialEncoding.h"
#include "MetalResourceRuntimeInternal.h"

#include <cmath>

namespace Horo::Render::Detail {
    namespace {
        std::uint64_t MaterialGraphInstance(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) {
            const auto found = std::ranges::find(request.resources, id, &RenderGraphResourceInstance::resource);
            return found == request.resources.end() ? 0 : found->instance;
        }

        bool WholeRasterView(const MetalTextureViewInstance &view, const MetalTextureInstance &source, FramebufferExtent extent) {
            return view.sourceInstance == &source && view.sourceTexture == source.texture && source.descriptor.extent == extent &&
                   source.descriptor.sampleCount == 1 && view.descriptor.baseMip == 0 && view.descriptor.mipCount == 1 &&
                   view.descriptor.baseLayer == 0 && view.descriptor.layerCount == 1;
        }

        bool MaterialFormatsMatch(const MetalMaterialBinding &material, const RenderGraphMaterialRaster &raster,
                                  const MetalRasterAttachments &attachments) {
            const auto &state = material.state;
            if (state.depthOnly != raster.depthOnly || state.sampleCount != 1 || state.viewRouting != RenderViewRouting::SingleView)
                return false;
            if (attachments.color && state.colorFormat != attachments.color->format)
                return false;
            if (state.depthFormat.has_value() != (attachments.depth != nullptr))
                return false;
            return !attachments.depth || *state.depthFormat == attachments.depth->format;
        }

        bool ImplicitParameters(const MaterialBindingDescriptor &descriptor, const MetalMaterialSlot &slot) {
            for (const auto &mapping : descriptor.layout.targetBindings) {
                if (mapping.nativeBinding != slot.binding || !mapping.active)
                    continue;
                const auto resource = std::ranges::find(descriptor.resources, mapping.id, &MaterialResourceBinding::binding);
                if (resource != descriptor.resources.end() && std::holds_alternative<MaterialParameterBinding>(resource->value))
                    return true;
            }
            return false;
        }

        bool BufferReadMatches(const RenderGraphMaterialRaster &raster, const MetalMaterialSlot &slot,
                               const RenderGraphExecutionRequest &request) {
            for (const auto &read : raster.reads)
                if (read.buffer.IsValid() && MaterialGraphInstance(request, read.resource) == Identity(slot.sourceBuffer))
                    return true;
            return false;
        }

        bool TextureReadMatches(const RenderGraphMaterialRaster &raster, const MetalMaterialSlot &slot,
                                const RenderGraphExecutionRequest &request, const std::unordered_set<MetalTextureViewInstance *> &views) {
            for (const auto &read : raster.reads) {
                if (!read.texture.IsValid())
                    continue;
                const auto mapped = std::ranges::find(request.views, read.texture, &RenderGraphTextureViewInstance::view);
                if (mapped == request.views.end())
                    continue;
                auto *view = Decode<MetalTextureViewInstance>(mapped->instance);
                if (!views.contains(view))
                    continue;
                if (view->sourceInstance == slot.sourceTextureInstance && view->texture == slot.texture &&
                    MaterialGraphInstance(request, read.resource) == Identity(slot.sourceTextureInstance))
                    return true;
            }
            return false;
        }

        bool MeshGraphInputs(const MetalMeshInstance &mesh, const RenderGraphMaterialDraw &draw, const RenderGraphExecutionRequest &request,
                             const std::unordered_set<MetalBufferInstance *> &buffers) {
            auto *vertices = Decode<MetalBufferInstance>(MaterialGraphInstance(request, draw.vertices));
            auto *indices = Decode<MetalBufferInstance>(MaterialGraphInstance(request, draw.indices));
            if (!buffers.contains(vertices) || !buffers.contains(indices) || vertices->use.retired || indices->use.retired)
                return false;
            return mesh.vertexBuffer == vertices->buffer && mesh.indexBuffer == indices->buffer &&
                   mesh.descriptor.vertexStride == sizeof(MeshVertex);
        }

        bool RasterRegionValid(const RenderGraphMaterialRaster &raster) noexcept {
            if (!raster.region)
                return true;
            const auto &r = *raster.region;
            return r.width != 0 && r.height != 0 && r.x < raster.extent.width && r.y < raster.extent.height &&
                   r.width <= raster.extent.width - r.x && r.height <= raster.extent.height - r.y;
        }

        bool RasterAttachmentPolicy(const RenderGraphMaterialRaster &raster) noexcept {
            if (raster.depthOnly)
                return raster.depth.IsValid() && raster.depthView.IsValid() && !raster.color.IsValid() && !raster.colorView.IsValid();
            return raster.color.IsValid() && raster.colorView.IsValid();
        }

        bool RasterScalarsValid(const RenderGraphMaterialRaster &raster) noexcept {
            if (!raster.extent.IsValid() || !raster.colorOperations.IsValid() || !RasterRegionValid(raster))
                return false;
            if (raster.depthClear && (!std::isfinite(*raster.depthClear) || *raster.depthClear < 0 || *raster.depthClear > 1))
                return false;
            return RasterAttachmentPolicy(raster);
        }

        struct MaterialEncoder final {
            __strong MTLRenderPassDescriptor *pass;
            __strong id<MTLRenderCommandEncoder> encoder{nil};

            ~MaterialEncoder() {
                [encoder endEncoding];
                pass.colorAttachments[0].texture = nil;
                pass.depthAttachment.texture = nil;
                pass.stencilAttachment.texture = nil;
            }
        };

        void ConfigureRasterAttachments(MTLRenderPassDescriptor *pass, const RenderGraphMaterialRaster &raster,
                                        const MetalRasterAttachments &attachments) {
            if (attachments.color) {
                pass.colorAttachments[0].texture = attachments.color->texture;
                ConfigureMetalColorAttachment(pass.colorAttachments[0], raster.colorOperations);
            }
            if (attachments.depth) {
                pass.depthAttachment.texture = attachments.depth->texture;
                pass.depthAttachment.loadAction = raster.depthClear ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
                pass.depthAttachment.clearDepth = raster.depthClear.value_or(1.0F);
            }
        }
    }  // namespace

    Result<MetalTextureViewInstance *> MetalResourceRuntime::Impl::RasterView(const RenderGraphResourceId resource,
                                                                              const RenderTextureViewHandle view,
                                                                              const RenderGraphExecutionRequest &request,
                                                                              const FramebufferExtent extent) const {
        using Outcome = Result<MetalTextureViewInstance *>;
        const auto mapped = std::ranges::find(request.views, view, &RenderGraphTextureViewInstance::view);
        if (mapped == request.views.end())
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        auto *nativeView = Decode<MetalTextureViewInstance>(mapped->instance);
        auto *source = Decode<MetalTextureInstance>(MaterialGraphInstance(request, resource));
        if (!textureViews.contains(nativeView) || !textures.contains(source) || source->use.retired)
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (!WholeRasterView(*nativeView, *source, extent) ||
            !HasTextureUsage(source->descriptor.usage, RenderTextureUsage::RenderAttachment))
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        return Outcome::Success(nativeView);
    }

    Result<MetalRasterAttachments> MetalResourceRuntime::Impl::RasterAttachments(const RenderGraphMaterialRaster &raster,
                                                                                 const RenderGraphExecutionRequest &request) const {
        MetalRasterAttachments result;
        if (!raster.depthOnly) {
            const auto view = RasterView(raster.color, raster.colorView, request, raster.extent);
            if (view.HasError())
                return Result<MetalRasterAttachments>::Failure(view.ErrorValue());
            result.color = view.Value();
            if (result.color->aspect != RenderTextureAspect::Color)
                return Result<MetalRasterAttachments>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        }
        if (raster.depth.IsValid()) {
            const auto view = RasterView(raster.depth, raster.depthView, request, raster.extent);
            if (view.HasError())
                return Result<MetalRasterAttachments>::Failure(view.ErrorValue());
            result.depth = view.Value();
            if (result.depth->aspect != RenderTextureAspect::Depth)
                return Result<MetalRasterAttachments>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }
        return Result<MetalRasterAttachments>::Success(result);
    }

    bool MetalResourceRuntime::Impl::RasterSlotRead(const RenderGraphMaterialRaster &raster, const ResidentMaterialBinding &resident,
                                                    const MetalMaterialSlot &slot, const RenderGraphExecutionRequest &request) const {
        if (slot.kind == MaterialResidentResourceKind::Buffer) {
            if (!buffers.contains(slot.sourceBuffer))
                return false;
            if (slot.sourceBuffer->use.retired)
                return false;
            return ImplicitParameters(resident.Descriptor(), slot) || BufferReadMatches(raster, slot, request);
        }
        if (slot.kind == MaterialResidentResourceKind::TextureView) {
            if (!textures.contains(slot.sourceTextureInstance))
                return false;
            return !slot.sourceTextureInstance->use.retired && TextureReadMatches(raster, slot, request, textureViews);
        }
        return true;
    }

    Result<void> MetalResourceRuntime::Impl::RasterMaterialReads(const RenderGraphMaterialRaster &raster,
                                                                 const ResidentMaterialBinding &resident,
                                                                 const MetalMaterialBinding &material,
                                                                 const RenderGraphExecutionRequest &request) const {
        for (const auto &slot : std::span{material.slots}.first(material.count))
            if (!RasterSlotRead(raster, resident, slot, request))
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        return Result<void>::Success();
    }

    Result<const MetalMaterialBinding *> MetalResourceRuntime::Impl::RasterMaterial(const RenderGraphMaterialDraw &draw) const {
        using Outcome = Result<const MetalMaterialBinding *>;
        if (!draw.material)
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto *material = dynamic_cast<const MetalMaterialBinding *>(&draw.material->BackendBinding().NativeBinding());
        if (!material || material->owner != this || material->deviceIncarnation != deviceIncarnation ||
            !graphicsPipelines.contains(material->sourcePipeline))
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (material->state != material->sourcePipeline->state ||
            !SameMetalMaterialLayout(draw.material->Descriptor().layout, material->sourcePipeline->layout))
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        return Outcome::Success(material);
    }

    Result<void> MetalResourceRuntime::Impl::RasterDraw(const RenderGraphMaterialRaster &raster, const RenderGraphMaterialDraw &draw,
                                                        const RenderGraphExecutionRequest &request,
                                                        const MetalRasterAttachments &attachments) const {
        const auto admitted = RasterMaterial(draw);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        const auto *material = admitted.Value();
        if (!MaterialFormatsMatch(*material, raster, attachments))
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto mapped = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
        if (mapped == request.meshes.end())
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto *mesh = Decode<MetalMeshInstance>(mapped->instance);
        if (!meshes.contains(const_cast<MetalMeshInstance *>(mesh)) || !MeshGraphInputs(*mesh, draw, request, buffers))
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        return RasterMaterialReads(raster, *draw.material, *material, request);
    }

    /** @copydoc MetalResourceRuntime::ValidateMaterialRaster */
    Result<void> MetalResourceRuntime::ValidateMaterialRaster(const RenderGraphMaterialRaster &raster,
                                                              const RenderGraphExecutionRequest &request) const {
        if (!impl_->device || !impl_->deviceIncarnation || !impl_->vfxPass || !RasterScalarsValid(raster) ||
            raster.draws.size() > RenderGraphMaterialRaster::MaximumDraws)
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto attachments = impl_->RasterAttachments(raster, request);
        if (attachments.HasError())
            return Result<void>::Failure(attachments.ErrorValue());
        for (const auto &draw : raster.draws)
            if (const auto valid = impl_->RasterDraw(raster, draw, request, attachments.Value()); valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    void MetalResourceRuntime::Impl::TrackRasterMaterialUse(const MetalMaterialBinding &material, id<MTLCommandBuffer> commands) noexcept {
        for (const auto &slot : std::span{material.slots}.first(material.count)) {
            if (slot.sourceBuffer)
                TrackUse(slot.sourceBuffer->use, commands);
            if (slot.sourceTextureInstance)
                TrackUse(slot.sourceTextureInstance->use, commands);
        }
    }

    /** @copydoc MetalResourceRuntime::ExecuteMaterialRaster */
    Result<void> MetalResourceRuntime::ExecuteMaterialRaster(void *commands, const RenderGraphMaterialRaster &raster,
                                                             const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateMaterialRaster(raster, request); valid.HasError())
            return valid;
        if (!commands)
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        const auto attachments = impl_->RasterAttachments(raster, request).Value();
        MaterialEncoder scope{impl_->vfxPass};
        ConfigureRasterAttachments(scope.pass, raster, attachments);
        TrackParallelGraphUse(commands, request.resources);
        scope.encoder = [(__bridge id<MTLCommandBuffer>)commands renderCommandEncoderWithDescriptor:scope.pass];
        if (!scope.encoder)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        const auto region = raster.region.value_or(RenderMaterialRegion{0, 0, raster.extent.width, raster.extent.height});
        [scope.encoder setViewport:MTLViewport{static_cast<double>(region.x), static_cast<double>(region.y),
                                               static_cast<double>(region.width), static_cast<double>(region.height), 0, 1}];
        [scope.encoder setScissorRect:MTLScissorRect{region.x, region.y, region.width, region.height}];
        for (const auto &draw : raster.draws) {
            const auto *material = dynamic_cast<const MetalMaterialBinding *>(&draw.material->BackendBinding().NativeBinding());
            const auto mapped = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
            const auto *mesh = Decode<MetalMeshInstance>(mapped->instance);
            impl_->TrackRasterMaterialUse(*material, (__bridge id<MTLCommandBuffer>)commands);
            BindMetalMaterial(scope.encoder, *material);
            [scope.encoder setVertexBuffer:mesh->vertexBuffer offset:0 atIndex:MetalMeshVertexBufferIndex];
            [scope.encoder
                drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                           indexCount:mesh->descriptor.indexCount
                            indexType:mesh->descriptor.indexFormat == RenderIndexFormat::UInt16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                          indexBuffer:mesh->indexBuffer
                    indexBufferOffset:0];
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
