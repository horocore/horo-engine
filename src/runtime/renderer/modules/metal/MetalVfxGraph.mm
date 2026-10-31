#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves an exact frontend-authored graph mapping before looking in the canonical native resident set. */
        std::uint64_t Instance(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) {
            if (id.owner != request.graph.Owner() || id.value == 0 || id.value > request.resources.size())
                return 0;
            const auto &resource = request.resources[id.value - 1];
            return resource.resource == id ? resource.instance : 0;
        }

        bool Visible(const ShaderStageVisibility stages, const ShaderStageVisibility stage) noexcept {
            return (static_cast<unsigned>(stages) & static_cast<unsigned>(stage)) != 0;
        }

        /** @brief Ends the owner encoder and removes staging references without releasing any command-completion ownership. */
        struct EncodingScope final {
            __strong MTLRenderPassDescriptor *pass;
            __strong id<MTLRenderCommandEncoder> encoder{nil};

            ~EncodingScope() {
                [encoder endEncoding];
                pass.colorAttachments[0].texture = nil;
                pass.depthAttachment.texture = nil;
                pass.stencilAttachment.texture = nil;
            }
        };

        bool ForwardState(const RenderGraphicsPipelineState &state, const VfxGraphStage stage) {
            if (!state.depthTest || !state.depthFormat || state.sampleCount != 1 || state.viewRouting != RenderViewRouting::SingleView)
                return false;
            switch (stage) {
                case VfxGraphStage::OpaqueForward:
                    return state.depthWrite && state.blend == RenderBlendMode::Disabled;
                case VfxGraphStage::TranslucentForward:
                    return !state.depthWrite && state.blend == RenderBlendMode::Alpha;
                case VfxGraphStage::AdditiveForward:
                    return !state.depthWrite && state.blend == RenderBlendMode::Additive;
                default:
                    return false;
            }
        }

        /** @brief Admits additional uniform backing only when it is canonical immutable packed material data. */
        bool ImmutableConstants(const MaterialBindingDescriptor &descriptor, const MetalMaterialSlot &slot) {
            for (const auto &mapping : descriptor.layout.targetBindings) {
                if (!mapping.active || mapping.generatedHelperIndex != 0 || mapping.nativeBinding != slot.binding ||
                    mapping.id == VfxShaderParameterIds::Instances)
                    continue;
                const auto logical = std::ranges::find(descriptor.resources, mapping.id, &MaterialResourceBinding::binding);
                if (logical != descriptor.resources.end() && std::holds_alternative<MaterialParameterBinding>(logical->value))
                    return true;
            }
            return false;
        }

        /** @brief Applies the already-prepared canonical material without allocating descriptor or argument storage. */
        void BindMaterial(id<MTLRenderCommandEncoder> encoder, const MetalMaterialBinding &material) {
            [encoder setRenderPipelineState:material.pipeline];
            [encoder setDepthStencilState:material.depth];
            [encoder setCullMode:material.state.cull == RenderCullMode::None    ? MTLCullModeNone
                                 : material.state.cull == RenderCullMode::Front ? MTLCullModeFront
                                                                                : MTLCullModeBack];
            [encoder setFrontFacingWinding:material.state.frontFace == RenderFrontFace::Clockwise ? MTLWindingClockwise
                                                                                                  : MTLWindingCounterClockwise];
            for (const auto &slot : std::span{material.slots}.first(material.count)) {
                if (slot.kind == MaterialResidentResourceKind::Buffer) {
                    if (Visible(slot.stages, ShaderStageVisibility::Vertex))
                        [encoder setVertexBuffer:slot.buffer offset:slot.offset atIndex:slot.binding];
                    if (Visible(slot.stages, ShaderStageVisibility::Fragment))
                        [encoder setFragmentBuffer:slot.buffer offset:slot.offset atIndex:slot.binding];
                } else if (slot.kind == MaterialResidentResourceKind::TextureView) {
                    if (Visible(slot.stages, ShaderStageVisibility::Vertex))
                        [encoder setVertexTexture:slot.texture atIndex:slot.binding];
                    if (Visible(slot.stages, ShaderStageVisibility::Fragment))
                        [encoder setFragmentTexture:slot.texture atIndex:slot.binding];
                } else {
                    if (Visible(slot.stages, ShaderStageVisibility::Vertex))
                        [encoder setVertexSamplerState:slot.sampler atIndex:slot.binding];
                    if (Visible(slot.stages, ShaderStageVisibility::Fragment))
                        [encoder setFragmentSamplerState:slot.sampler atIndex:slot.binding];
                }
            }
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::Impl::VfxTarget */
    Result<MetalRenderTargetInstance *> MetalResourceRuntime::Impl::VfxTarget(const RenderGraphVfx &workload,
                                                                              const RenderGraphExecutionRequest &request) const {
        using Target = Result<MetalRenderTargetInstance *>;
        const auto mapped = std::ranges::find(request.targets, workload.target, &RenderGraphTargetInstance::target);
        auto *target = mapped == request.targets.end() ? nullptr : Decode<MetalRenderTargetInstance>(mapped->instance);
        auto *color = Decode<MetalTextureInstance>(Instance(request, workload.color));
        auto *depth = Decode<MetalTextureInstance>(Instance(request, workload.depth));
        if (device == nil || deviceIncarnation == 0 || vfxPass == nil || !renderTargets.contains(target) || !textures.contains(color) ||
            !textures.contains(depth) || color == depth || color->use.retired || depth->use.retired ||
            target->colorSourceTexture != color->texture || target->depthSourceTexture != depth->texture || !target->colorTexture ||
            !target->depthTexture || target->descriptor.extent != color->descriptor.extent ||
            target->descriptor.extent != depth->descriptor.extent)
            return Target::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return Target::Success(target);
    }

    /** @copydoc MetalResourceRuntime::Impl::VfxDraw */
    Result<void> MetalResourceRuntime::Impl::VfxDraw(const RenderGraphVfx &workload, const VfxPreparedDraw &draw,
                                                     const RenderGraphExecutionRequest &request,
                                                     const MetalRenderTargetInstance &target) const {
        if (!draw.slot || draw.slot->ValidateDraw(workload, draw).HasError())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto &resident = *draw.slot->Material();
        const auto *material = dynamic_cast<const MetalMaterialBinding *>(&resident.BackendBinding().NativeBinding());
        const auto mappedMesh = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
        auto *const mesh = mappedMesh == request.meshes.end() ? nullptr : Decode<MetalMeshInstance>(mappedMesh->instance);
        auto *parameters = Decode<MetalBufferInstance>(Instance(request, draw.parameters));
        auto *vertex = Decode<MetalBufferInstance>(Instance(request, draw.vertices));
        auto *index = Decode<MetalBufferInstance>(Instance(request, draw.indices));
        if (!material || material->owner != this || material->deviceIncarnation != deviceIncarnation ||
            material->count > material->slots.size() || !graphicsPipelines.contains(material->sourcePipeline) ||
            material->pipeline != material->sourcePipeline->pipeline ||
            !SameMetalMaterialLayout(resident.Descriptor().layout, material->sourcePipeline->layout) ||
            VfxShaderParameterBytes(resident.Descriptor().layout).HasError() || !ForwardState(material->state, workload.stage) ||
            !meshes.contains(mesh) || !buffers.contains(vertex) || !buffers.contains(index) || !buffers.contains(parameters) ||
            vertex->use.retired || index->use.retired || parameters->use.retired || mesh->vertexBuffer != vertex->buffer ||
            mesh->indexBuffer != index->buffer || mesh->descriptor.vertexStride != sizeof(MeshVertex) || mesh->descriptor.indexCount == 0 ||
            parameters->deviceIncarnation != deviceIncarnation || !parameters->vfxPublished || parameters->vfxRevision != draw.revision ||
            parameters->vfxInstances != draw.instanceCount)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto *color = Decode<MetalTextureInstance>(Instance(request, workload.color));
        const auto *depth = Decode<MetalTextureInstance>(Instance(request, workload.depth));
        if (material->state.colorFormat != color->descriptor.format || material->state.depthFormat != depth->descriptor.format)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        std::size_t instanceBindings{}, samples{};
        for (const auto &slot : std::span{material->slots}.first(material->count)) {
            if (slot.binding >= 31)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (slot.kind == MaterialResidentResourceKind::Buffer) {
                if (slot.buffer == parameters->buffer) {
                    if (++instanceBindings > 1 || slot.offset != 0 || slot.byteCount != parameters->descriptor.byteSize)
                        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                } else if (!ImmutableConstants(resident.Descriptor(), slot)) {
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                }
            } else if (slot.kind == MaterialResidentResourceKind::TextureView) {
                auto *const sample = Decode<MetalTextureInstance>(Instance(request, draw.sampledTexture));
                if (++samples > 1 || !textures.contains(sample) || sample->use.retired || slot.sourceTexture != sample->texture ||
                    slot.sourceTexture == target.colorSourceTexture || slot.sourceTexture == target.depthSourceTexture)
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            } else if (slot.kind != MaterialResidentResourceKind::Sampler || slot.sampler == nil) {
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            }
        }
        return instanceBindings == 1 && ((samples != 0) == draw.sampledTexture.IsValid())
                   ? Result<void>::Success()
                   : Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    }

    /** @copydoc MetalResourceRuntime::ValidateVfxWorkload */
    Result<void> MetalResourceRuntime::ValidateVfxWorkload(const RenderGraphVfx &workload,
                                                           const RenderGraphExecutionRequest &request) const {
        const auto target = impl_->VfxTarget(workload, request);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        if (!workload.order || !workload.order->nativePrepared || workload.order->draws.empty())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        for (const auto &draw : workload.order->draws)
            if (const auto valid = impl_->VfxDraw(workload, draw, request, *target.Value()); valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::ExecuteVfxWorkload */
    Result<void> MetalResourceRuntime::ExecuteVfxWorkload(void *commands, const RenderGraphVfx &workload,
                                                          const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateVfxWorkload(workload, request); valid.HasError())
            return valid;
        auto *target = impl_->VfxTarget(workload, request).Value();
        MTLRenderPassDescriptor *pass = impl_->vfxPass;
        EncodingScope scope{pass};
        pass.colorAttachments[0].texture = target->colorTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = target->depthTexture;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (target->depthTexture.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8 ||
            target->depthTexture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            pass.stencilAttachment.texture = target->depthTexture;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)commands;
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        scope.encoder = encoder;
        if (!encoder)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        const auto extent = target->descriptor.extent;
        [encoder setViewport:MTLViewport{0, 0, static_cast<double>(extent.width), static_cast<double>(extent.height), 0, 1}];
        [encoder setScissorRect:MTLScissorRect{0, 0, extent.width, extent.height}];
        TrackParallelGraphUse(commands, request.resources);
        for (const auto &draw : workload.order->draws) {
            const auto *material = dynamic_cast<const MetalMaterialBinding *>(&draw.slot->Material()->BackendBinding().NativeBinding());
            const auto mapped = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
            const auto *mesh = Decode<MetalMeshInstance>(mapped->instance);
            BindMaterial(encoder, *material);
            [encoder setVertexBuffer:mesh->vertexBuffer offset:0 atIndex:MetalMeshVertexBufferIndex];
            [encoder
                drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                           indexCount:mesh->descriptor.indexCount
                            indexType:mesh->descriptor.indexFormat == RenderIndexFormat::UInt16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                          indexBuffer:mesh->indexBuffer
                    indexBufferOffset:0
                        instanceCount:draw.instanceCount];
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
