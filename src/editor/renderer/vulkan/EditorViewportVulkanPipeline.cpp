#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::CreatePipeline */
    Result<void> EditorViewportRendererVulkan::CreatePipeline() {
        using namespace Render;
        const auto encoded = EncodeReflectedShaderArtifact(shader_.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        const auto checked = LoadEditorVulkanViewportCook(shader_.payload, encoded.Value());
        if (checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (auto stages = CreateProgramStages(shader_); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .depthFormat = RenderTextureFormat::Depth32Float};
        const auto pipeline = CreatePipelineState(state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        shader_.pipeline = pipeline.Value();
        auto overlayState = state;
        overlayState.depthWrite = false;
        const auto overlay = CreatePipelineState(overlayState);
        if (overlay.HasError())
            return Result<void>::Failure(overlay.ErrorValue());
        shader_.overlayPipeline = overlay.Value();
        return CreateMaterialLayout(shader_.metadata.reflection);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateProgramStages */
    Result<void> EditorViewportRendererVulkan::CreateProgramStages(ShaderResources &program) {
        using namespace Render;
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = program.metadata.reflection;
        const auto stage = [&](const ShaderStage kind, const char *entry) {
            return frontend_.CreateShaderModule({target, kind, entry, program.metadata.cookedArtifactKey,
                                                 reflection.interfaceCompatibility},
                                                program.payload);
        };
        const auto vertex = stage(ShaderStage::Vertex, "VSMain");
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        program.vertex = vertex.Value().handle;
        const auto fragment = stage(ShaderStage::Fragment, "PSMain");
        if (fragment.HasError())
            return Result<void>::Failure(fragment.ErrorValue());
        program.fragment = fragment.Value().handle;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::CreatePipelineState */
    Result<Render::RenderPipelineHandle> EditorViewportRendererVulkan::CreatePipelineState(
        const Render::RenderGraphicsPipelineState &state) {
        return CreatePipelineState(shader_, EditorVulkanViewportShaderManifest(), state);
    }

    /** @copydoc EditorViewportRendererVulkan::CreatePipelineState */
    Result<Render::RenderPipelineHandle> EditorViewportRendererVulkan::CreatePipelineState(
        const ShaderResources &program, const Render::ShaderManifest &manifest, const Render::RenderGraphicsPipelineState &state) {
        using namespace Render;
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = program.metadata.reflection;
        const auto device = frontend_.QueryPipelineDeviceIdentity();
        if (device.HasError())
            return Result<RenderPipelineHandle>::Failure(device.ErrorValue());
        const auto stateKey = ComputeRenderGraphicsPipelineStateDigest(state);
        if (stateKey.HasError())
            return Result<RenderPipelineHandle>::Failure(stateKey.ErrorValue());
        const auto &identity = device.Value();
        const PipelineCacheCompatibility compatibility{1,
                                                       identity.backend,
                                                       identity.backendModuleVersion,
                                                       identity.adapter,
                                                       identity.deviceCompatibilityDigest,
                                                       identity.driverIdentity,
                                                       identity.driverVersion,
                                                       target.backend,
                                                       target.payloadFormat,
                                                       program.metadata.cookedArtifactKey,
                                                       reflection.interfaceCompatibility,
                                                       stateKey.Value()};
        const auto pipeline =
            frontend_.CreateGraphicsPipeline({program.vertex, program.fragment, manifest, target, reflection, state, compatibility});
        if (pipeline.HasError())
            return Result<RenderPipelineHandle>::Failure(pipeline.ErrorValue());
        return Result<RenderPipelineHandle>::Success(pipeline.Value().handle);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateMaterialLayout */
    Result<void> EditorViewportRendererVulkan::CreateMaterialLayout(const Render::NormalizedShaderReflection &reflection) {
        return CreateMaterialLayout(shader_, reflection);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateMaterialLayout */
    Result<void> EditorViewportRendererVulkan::CreateMaterialLayout(ShaderResources &program,
                                                                    const Render::NormalizedShaderReflection &reflection) {
        using namespace Render;
        const auto layout = PrepareMaterialBindingLayout(reflection);
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        program.layout = layout.Value();
        const auto bytes = ComputeMaterialParameterBufferByteSize(program.layout, {1});
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > MaximumUniformBytes)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        const auto nativeBytes = Render::Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(frontend_,
                                                                                                            {.pipeline = program.pipeline,
                                                                                                             .layout = program.layout},
                                                                                                            {1});
        if (nativeBytes.HasError())
            return Result<void>::Failure(nativeBytes.ErrorValue());
        if (nativeBytes.Value() < bytes.Value() || nativeBytes.Value() > MaximumUniformBytes)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        program.parameterBytes = bytes.Value();
        program.uniformBytes = nativeBytes.Value();
        auto materials = MaterialBindingTable::Create(program.pipeline.owner, frontend_,
                                                      {.maximumGenerations = device_.frames * (RenderMaterialPass::MaximumDraws + 1)});
        if (materials.HasError())
            return Result<void>::Failure(materials.ErrorValue());
        program.materials = std::move(materials).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::CreateDrawSlots */
    Result<void> EditorViewportRendererVulkan::CreateDrawSlots() {
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::size_t index = 0; index < Render::RenderMaterialPass::MaximumDraws; ++index) {
                const auto created = CreateUniformSlot(slots_[slot].draws[index],
                                                       slot * (Render::RenderMaterialPass::MaximumDraws + 1) + index + 1, shader_.pipeline);
                if (created.HasError())
                    return created;
            }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::CreateUniformSlot */
    Result<void> EditorViewportRendererVulkan::CreateUniformSlot(DrawSlot &draw, const std::uint64_t material,
                                                                 const Render::RenderPipelineHandle pipeline) {
        return CreateUniformSlot(shader_, draw, material, pipeline);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateUniformSlot */
    Result<void> EditorViewportRendererVulkan::CreateUniformSlot(const ShaderResources &program, DrawSlot &draw,
                                                                 const std::uint64_t material,
                                                                 const Render::RenderPipelineHandle pipeline) {
        using namespace Render;
        const std::array<std::byte, MaximumUniformBytes> zero{};
        const auto buffer = frontend_.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                   {program.uniformBytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible},
                                                   std::span{zero}.first(program.uniformBytes));
        if (buffer.HasError())
            return Result<void>::Failure(buffer.ErrorValue());
        draw.buffer = buffer.Value().handle;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        if (const auto ready = frontend_.ResourceOperationResult(buffer.Value().operation); ready.HasError())
            return ready;
        const auto native = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend_, draw.buffer);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        draw.instance = native.Value();
        const auto published =
            program.materials->Publish({.material = {material},
                                        .revision = 1,
                                        .pipeline = pipeline,
                                        .layout = program.layout,
                                        .resources = {{{1}, 0, MaterialBufferBinding{draw.buffer, 0, program.uniformBytes}}}});
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        const auto acquired = program.materials->Acquire(published.Value().binding);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        draw.material = acquired.Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareXrProgram */
    Result<void> EditorViewportRendererVulkan::PrepareXrProgram(EditorVulkanMultiviewCook cooked) {
        using namespace Render;
        if (owner_ != std::this_thread::get_id() || !initialized_ || recordedEver_ || xrShader_.vertex.IsValid())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto encoded = EncodeReflectedShaderArtifact(cooked.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (const auto admitted = LoadEditorVulkanMultiviewCook(cooked.payload, encoded.Value()); admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        xrShader_.payload = std::move(cooked.payload);
        xrShader_.metadata = std::move(cooked.metadata);
        if (auto stages = CreateProgramStages(xrShader_); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .depthFormat = RenderTextureFormat::Depth32Float};
        const auto pipeline = CreatePipelineState(xrShader_, EditorVulkanMultiviewShaderManifest(), state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        xrShader_.pipeline = pipeline.Value();
        if (auto layout = CreateMaterialLayout(xrShader_, xrShader_.metadata.reflection); layout.HasError())
            return layout;
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::size_t index = 0; index < RenderMaterialPass::MaximumDraws; ++index)
                if (auto prepared = CreateUniformSlot(xrShader_, xrDrawSlots_[slot][index],
                                                      slot * RenderMaterialPass::MaximumDraws + index + 1, xrShader_.pipeline);
                    prepared.HasError())
                    return prepared;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
