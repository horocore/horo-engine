#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Reads an exact frontend-authored graph identity after portable request validation. */
        std::uint64_t Instance(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) {
            if (id.owner != request.graph.Owner() || id.value == 0 || id.value > request.resources.size())
                return 0;
            const auto &entry = request.resources[id.value - 1];
            return entry.resource == id ? entry.instance : 0;
        }

        /** @brief Admits only implemented forward depth/blend semantics, without selecting variants or pipelines. */
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

        /** @brief Additional material constants must be immutable canonical parameter data, never hidden dynamic graph reads. */
        bool ImmutableConstants(const MaterialBindingDescriptor &descriptor, const ShaderBindingId binding) {
            const auto found = std::ranges::find(descriptor.resources, binding, &MaterialResourceBinding::binding);
            return found != descriptor.resources.end() && std::holds_alternative<MaterialParameterBinding>(found->value);
        }
    }  // namespace

    /** @copydoc VulkanGraphRuntime::Impl::PrepareVfxMaterialBindings */
    Result<void> VulkanGraphRuntime::Impl::PrepareVfxMaterialBindings(const MaterialBindingDescriptor &descriptor,
                                                                      const VulkanPreparedMaterialBinding &material,
                                                                      const VfxPreparedDraw &source, const VulkanMaterialTarget &target) {
        const auto &parameters = bindings[source.parameters.value - 1];
        std::size_t instanceBindings{}, samples{};
        for (const auto &resource : material.resources) {
            if (resource.buffer) {
                if (resource.buffer == parameters.buffer) {
                    if (++instanceBindings > 1 || resource.binding != VfxShaderParameterIds::Instances || resource.offset != 0 ||
                        resource.byteCount != parameters.bufferDescriptor.byteSize || resource.state != parameters.state)
                        return Result<void>::Failure(Invalid("VFX instance uniform has a foreign or aliased native range."));
                } else {
                    if (!ImmutableConstants(descriptor, resource.binding) ||
                        std::ranges::binary_search(identities, resource.state, std::less<>{}))
                        return Result<void>::Failure(Invalid("VFX material has a dynamic or graph-aliased constant input."));
                    const VulkanGraphResource constant{.buffer = resource.buffer,
                                                       .bufferDescriptor = resource.bufferDescriptor,
                                                       .state = resource.state};
                    const auto &submission = submissions.back().value;
                    const RenderGraphLogicalState input{RenderGraphSynchronizationAccess::Read,
                                                        RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                                        RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable,
                                                        submission.queue};
                    if (const auto prepared = MaterialState(constant, input, submission, true); prepared.HasError())
                        return Result<void>::Failure(prepared.ErrorValue());
                }
            } else if (resource.image) {
                if (!source.sampledTexture.IsValid() || ++samples > 1)
                    return Result<void>::Failure(Invalid("VFX sampled input is not declared by the graph."));
                const auto &sample = bindings[source.sampledTexture.value - 1];
                if (resource.image != sample.image || resource.state != sample.state || resource.image == target.color.image ||
                    resource.image == target.depth.image)
                    return Result<void>::Failure(Invalid("VFX sampled input differs from the exact graph texture generation."));
            } else {
                return Result<void>::Failure(Invalid("VFX material resource has no retained native backing."));
            }
        }
        if (instanceBindings != 1 || ((samples != 0) != source.sampledTexture.IsValid()))
            return Result<void>::Failure(Invalid("VFX material and graph resource roles differ."));
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::PrepareVfxDraw */
    Result<VulkanGraphRuntime::Impl::MaterialDraw> VulkanGraphRuntime::Impl::PrepareVfxDraw(const RenderGraphExecutionRequest &request,
                                                                                            const RenderGraphVfx &workload,
                                                                                            const VfxPreparedDraw &source,
                                                                                            const VulkanMaterialTarget &target,
                                                                                            VulkanCommandRetention &pins) {
        using Outcome = Result<MaterialDraw>;
        if (!source.slot || source.slot->ValidateDraw(workload, source).HasError())
            return Outcome::Failure(Invalid("VFX draw has no current immutable publication."));
        const auto &resident = *source.slot->Material();
        const auto *material = dynamic_cast<const VulkanPreparedMaterialBinding *>(&resident.BackendBinding().NativeBinding());
        if (!material || material->deviceIncarnation != deviceIncarnation || !ForwardState(material->state, workload.stage) ||
            material->state.colorFormat != target.color.textureDescriptor.format ||
            material->state.depthFormat != target.depth.textureDescriptor.format)
            return Outcome::Failure(Invalid("VFX material depth, blend or selected-device generation differs."));
        if (const auto valid = resources->ValidateVfxMaterial(*material, resident.Descriptor().layout); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto mapping = std::ranges::find(request.meshes, source.mesh, &RenderGraphMeshInstance::mesh);
        if (mapping == request.meshes.end())
            return Outcome::Failure(Invalid("VFX mesh has no exact frontend resource mapping."));
        const auto mesh = resources->MaterialCapture().CaptureMaterialMesh(mapping->instance, pins);
        if (mesh.HasError())
            return Outcome::Failure(mesh.ErrorValue());
        const auto &vertices = bindings[source.vertices.value - 1];
        const auto &indices = bindings[source.indices.value - 1];
        const auto &parameters = bindings[source.parameters.value - 1];
        const auto publication = resources->VfxPublication(Instance(request, source.parameters));
        if (mesh.Value().vertex.buffer != vertices.buffer || mesh.Value().vertex.state != vertices.state ||
            mesh.Value().index.buffer != indices.buffer || mesh.Value().index.state != indices.state ||
            mesh.Value().descriptor.vertexStride != sizeof(MeshVertex) || mesh.Value().descriptor.indexCount == 0 ||
            publication.HasError() || !publication.Value().published || publication.Value().buffer != parameters.buffer ||
            publication.Value().revision != source.revision || publication.Value().instances != source.instanceCount)
            return Outcome::Failure(Invalid("VFX draw differs from its captured mesh or accepted native parameter revision."));
        if (const auto bindingsValid = PrepareVfxMaterialBindings(resident.Descriptor(), *material, source, target);
            bindingsValid.HasError())
            return Outcome::Failure(bindingsValid.ErrorValue());
        return Outcome::Success({mesh.Value(), material, source.instanceCount});
    }

    /** @copydoc VulkanGraphRuntime::Impl::PrepareVfx */
    Result<void> VulkanGraphRuntime::Impl::PrepareVfx(const RenderGraphExecutionRequest &request, const std::size_t pass,
                                                      VulkanCommandRetention &pins) {
        const auto &workload = std::get<RenderGraphVfx>(request.workloads[pass].workload);
        if (!workload.order || !workload.order->nativePrepared || workload.order->draws.empty() ||
            workload.order->draws.size() > vfxDraws.capacity() - vfxDraws.size() || !native.bindPipeline || !native.bindSets ||
            !native.bindVertices || !native.bindIndex || !native.viewport || !native.scissor || !native.drawIndexed)
            return Result<void>::Failure(Unsupported("VFX requires a complete bounded native draw order and graphics dispatch."));
        const auto mapping = std::ranges::find(request.targets, workload.target, &RenderGraphTargetInstance::target);
        if (mapping == request.targets.end())
            return Result<void>::Failure(Invalid("VFX target has no exact frontend resource mapping."));
        const auto target = resources->MaterialCapture().CaptureMaterialTarget(mapping->instance, pins);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        const auto &color = bindings[workload.color.value - 1];
        const auto &depth = bindings[workload.depth.value - 1];
        if (!target.Value().depthView || target.Value().color.image != color.image || target.Value().color.state != color.state ||
            target.Value().depth.image != depth.image || target.Value().depth.state != depth.state || color.image == depth.image)
            return Result<void>::Failure(Invalid("VFX target attachments differ from the declared graph generations."));
        auto &prepared = passes[pass];
        prepared.vfxTarget = target.Value();
        prepared.vfxDraws.offset = vfxDraws.size();
        for (const auto &source : workload.order->draws) {
            const auto draw = PrepareVfxDraw(request, workload, source, target.Value(), pins);
            if (draw.HasError())
                return Result<void>::Failure(draw.ErrorValue());
            vfxDraws.push_back(draw.Value());
        }
        prepared.vfxDraws.count = vfxDraws.size() - prepared.vfxDraws.offset;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::EncodeVfx */
    void VulkanGraphRuntime::Impl::EncodeVfx(const VkCommandBuffer command, const std::size_t pass) const noexcept {
        const auto &prepared = passes[pass];
        const auto &target = prepared.vfxTarget;
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = target.colorView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = target.depthView,
                                              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const auto format = target.depth.textureDescriptor.format;
        const bool stencil = format == RenderTextureFormat::Depth24Stencil8 || format == RenderTextureFormat::Depth32FloatStencil8;
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {target.extent.width, target.extent.height}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color,
                                        .pDepthAttachment = &depth,
                                        .pStencilAttachment = stencil ? &depth : nullptr};
        native.beginRendering(command, &rendering);
        const VkViewport viewport{0, 0, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {target.extent.width, target.extent.height}};
        native.viewport(command, 0, 1, &viewport);
        native.scissor(command, 0, 1, &scissor);
        for (const auto &draw : std::span{vfxDraws}.subspan(prepared.vfxDraws.offset, prepared.vfxDraws.count)) {
            native.bindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->pipeline);
            if (!draw.material->sets.empty())
                native.bindSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->layout, 0,
                                static_cast<std::uint32_t>(draw.material->sets.size()), draw.material->sets.data(), 0, nullptr);
            const VkDeviceSize offset = 0;
            native.bindVertices(command, 0, 1, &draw.mesh.vertex.buffer, &offset);
            native.bindIndex(command, draw.mesh.index.buffer, 0,
                             draw.mesh.descriptor.indexFormat == RenderIndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            native.drawIndexed(command, draw.mesh.descriptor.indexCount, draw.instances, 0, 0, 0);
        }
        native.endRendering(command);
    }
}  // namespace Horo::Render::Detail
