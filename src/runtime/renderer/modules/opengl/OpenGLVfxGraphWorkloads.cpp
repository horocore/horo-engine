#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "OpenGLGraphWorkloads.h"
#include "OpenGLPreparedMaterialState.h"
#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves one exact graph token without assuming native integer width. */
        std::uint32_t VfxInstance(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) {
            if (id.owner != request.graph.Owner() || id.value == 0 || id.value > request.resources.size())
                return 0;
            const auto &resource = request.resources[id.value - 1];
            return resource.resource == id && resource.instance <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(resource.instance)
                       : 0;
        }

        /** @brief Checks material-owned fixed state against the existing forward stage's depth and blend contract. */
        bool VfxState(const RenderGraphicsPipelineState &state, const VfxGraphStage stage) {
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

        /** @brief Admits additional constants only when the canonical resident material owns immutable packed backing. */
        bool ImmutableVfxConstants(const MaterialBindingDescriptor &descriptor, const OpenGLMaterialSlot &slot,
                                   const OpenGLGraphResources &resources) {
            const auto buffer = resources.buffers.find(slot.object);
            if (buffer == resources.buffers.end() || slot.offset > buffer->second.byteSize ||
                slot.byteCount > buffer->second.byteSize - slot.offset)
                return false;
            for (const auto &mapping : descriptor.layout.targetBindings) {
                if (!mapping.active || mapping.generatedHelperIndex != 0 || mapping.nativeBinding != slot.binding ||
                    mapping.id == VfxShaderParameterIds::Instances)
                    continue;
                const auto resource = std::ranges::find(descriptor.resources, mapping.id, &MaterialResourceBinding::binding);
                if (resource != descriptor.resources.end() && std::holds_alternative<MaterialParameterBinding>(resource->value))
                    return true;
            }
            return false;
        }

        /** @brief Borrows the exact prepared target after checking both graph-local attachment identities. */
        Result<std::uint32_t> VfxTarget(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                        const RenderGraphVfx &workload) {
            const auto resolved = std::ranges::find(request.targets, workload.target, &RenderGraphTargetInstance::target);
            if (!resources.prepared || !resources.owner || !resources.deviceIncarnation || !resources.accepting || !*resources.accepting ||
                resolved == request.targets.end() || resolved->instance == 0 ||
                resolved->instance > std::numeric_limits<std::uint32_t>::max())
                return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto name = static_cast<std::uint32_t>(resolved->instance);
            const auto target = resources.prepared->materialTargets.find(name);
            const auto color = VfxInstance(request, workload.color), depth = VfxInstance(request, workload.depth);
            const auto colorTexture = resources.textures.find(color), depthTexture = resources.textures.find(depth);
            if (target == resources.prepared->materialTargets.end() || !color || !depth || color == depth ||
                target->second.colorTexture != color || target->second.depthTexture != depth || colorTexture == resources.textures.end() ||
                depthTexture == resources.textures.end() || colorTexture->second.destroyRequested ||
                depthTexture->second.destroyRequested || target->second.extent != colorTexture->second.descriptor.extent ||
                target->second.extent != depthTexture->second.descriptor.extent)
                return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            return Result<std::uint32_t>::Success(name);
        }

        /** @brief Validates exact instance, immutable-constant and sampled-input roles without issuing native commands. */
        Result<void> ValidateVfxMaterialSlots(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                              const VfxPreparedDraw &draw, const OpenGLMaterialTarget &target,
                                              const MaterialBindingDescriptor &descriptor, const OpenGLMaterialBinding &material,
                                              const std::uint32_t parameters, const std::size_t blockBytes) {
            std::size_t uniforms{}, samples{};
            for (const auto &slot : std::span{material.slots}.first(material.count)) {
                if (slot.binding >= 128 || slot.object == 0)
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
                if (slot.kind == MaterialResidentResourceKind::Buffer) {
                    if (slot.object != parameters) {
                        if (!ImmutableVfxConstants(descriptor, slot, resources))
                            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                        continue;
                    }
                    if (++uniforms != 1 || slot.offset != 0 || slot.byteCount != blockBytes)
                        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                } else {
                    const auto sample = resources.textures.find(slot.object);
                    if (++samples > 1 || slot.object != VfxInstance(request, draw.sampledTexture) || slot.object == target.colorTexture ||
                        slot.object == target.depthTexture || sample == resources.textures.end() || sample->second.destroyRequested ||
                        !resources.prepared->samplerDescriptors.contains(slot.sampler))
                        return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
                }
            }
            if (uniforms != 1 || (samples == 0) != !draw.sampledTexture.IsValid())
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return Result<void>::Success();
        }

        /** @brief Produces one borrowed immutable canonical material command after exact native publication checks. */
        Result<OpenGLMaterialDrawCommand> VfxCommand(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                                     const RenderGraphVfx &workload, const VfxPreparedDraw &draw,
                                                     const OpenGLMaterialTarget &target) {
            using Command = Result<OpenGLMaterialDrawCommand>;
            if (!draw.slot || draw.slot->ValidateDraw(workload, draw).HasError())
                return Command::Failure(MakeError(VfxFrameErrors::InvalidInput));
            const auto &resident = *draw.slot->Material();
            const auto *material = dynamic_cast<const OpenGLMaterialBinding *>(&resident.BackendBinding().NativeBinding());
            if (!material || material->owner != resources.owner || material->deviceIncarnation != resources.deviceIncarnation)
                return Command::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto pipeline = resources.prepared->graphicsPipelines.find(material->program);
            const auto resolved = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
            if (pipeline == resources.prepared->graphicsPipelines.end() || resolved == request.meshes.end() || resolved->instance == 0 ||
                resolved->instance > std::numeric_limits<std::uint32_t>::max() ||
                !SameMaterialLayout(pipeline->second.layout, resident.Descriptor().layout) ||
                VfxShaderParameterBytes(pipeline->second.layout).HasError() || !VfxState(pipeline->second.state, workload.stage) ||
                pipeline->second.state.colorFormat != target.colorFormat || pipeline->second.state.depthFormat != target.depthFormat ||
                pipeline->second.state.sampleCount != target.sampleCount)
                return Command::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto mesh = resources.prepared->meshDescriptors.find(static_cast<std::uint32_t>(resolved->instance));
            const auto parameters = VfxInstance(request, draw.parameters);
            const auto use = resources.prepared->uniformFrameUses.find(parameters);
            const auto descriptor = resources.buffers.find(parameters);
            if (mesh == resources.prepared->meshDescriptors.end() || mesh->second.vertices != VfxInstance(request, draw.vertices) ||
                mesh->second.indices != VfxInstance(request, draw.indices) || mesh->second.descriptor.indexCount == 0 ||
                mesh->second.descriptor.indexCount > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
                use == resources.prepared->uniformFrameUses.end() || descriptor == resources.buffers.end() || !use->second.published ||
                use->second.deviceIncarnation != resources.deviceIncarnation || use->second.revision != draw.revision ||
                use->second.instanceCount != draw.instanceCount || use->second.blockBytes != descriptor->second.byteSize)
                return Command::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (const auto slots = ValidateVfxMaterialSlots(request, resources, draw, target, resident.Descriptor(), *material, parameters,
                                                            use->second.blockBytes);
                slots.HasError())
                return Command::Failure(slots.ErrorValue());
            return Command::Success({material->program, mesh->first, mesh->second.descriptor.indexCount,
                                     mesh->second.descriptor.indexFormat, pipeline->second.state, pipeline->second.features,
                                     std::span{material->slots}.first(material->count), draw.instanceCount});
        }
    }  // namespace

    /** @copydoc ValidateOpenGLVfxWorkload */
    Result<void> ValidateOpenGLVfxWorkload(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                           const RenderGraphVfx &workload) {
        const auto target = VfxTarget(request, resources, workload);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        if (!workload.order)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto &attachment = resources.prepared->materialTargets.at(target.Value());
        for (const auto &draw : workload.order->draws)
            if (const auto command = VfxCommand(request, resources, workload, draw, attachment); command.HasError())
                return Result<void>::Failure(command.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc ExecuteOpenGLVfxWorkload */
    Result<void> ExecuteOpenGLVfxWorkload(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                          const OpenGLCommandFunctions &functions, const RenderGraphVfx &workload) {
        if (!functions.materialExecution.IsValid() || !functions.materialExecution.isAvailable())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
        const auto target = VfxTarget(request, resources, workload);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        // No map reference survives the injected material execution callback.
        const auto attachment = resources.prepared->materialTargets.at(target.Value());
        for (const auto &draw : workload.order->draws) {
            if (!resources.accepting || !*resources.accepting)
                return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
            const auto command = VfxCommand(request, resources, workload, draw, attachment);
            if (command.HasError())
                return Result<void>::Failure(command.ErrorValue());
            const RenderMaterialDrawInstance instance{command.Value().vertexArray,
                                                      &draw.slot->Material()->BackendBinding().NativeBinding()};
            const RenderMaterialExecutionRequest
                native{request.frame,           target.Value(), attachment.extent, {.loadOperation = AttachmentLoadOperation::Load}, {},
                       std::span{&instance, 1}, request.lease,  request.transfer};
            if (const auto encoded = functions.materialExecution.execute(native, std::span{&command.Value(), 1}); encoded.HasError())
                return encoded;
            if (!*resources.accepting)
                return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
