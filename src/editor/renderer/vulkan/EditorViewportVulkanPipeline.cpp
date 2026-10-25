#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::CreatePipeline */
    Result<void> EditorViewportRendererVulkan::CreatePipeline() {
        using namespace Render;
        const auto encoded = EncodeReflectedShaderArtifact(shader_.cooked.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        const auto checked = LoadEditorVulkanViewportCook(shader_.cooked.payload, encoded.Value());
        if (checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = shader_.cooked.metadata.reflection;
        const auto stage = [&](const ShaderStage kind, const char *entry) {
            return frontend_.CreateShaderModule({target, kind, entry, shader_.cooked.metadata.cookedArtifactKey,
                                                 reflection.interfaceCompatibility},
                                                shader_.cooked.payload);
        };
        const auto vertex = stage(ShaderStage::Vertex, "VSMain");
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        shader_.vertex = vertex.Value().handle;
        const auto fragment = stage(ShaderStage::Fragment, "PSMain");
        if (fragment.HasError())
            return Result<void>::Failure(fragment.ErrorValue());
        shader_.fragment = fragment.Value().handle;
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
        return CreateMaterialLayout(reflection);
    }

    /** @copydoc EditorViewportRendererVulkan::CreatePipelineState */
    Result<Render::RenderPipelineHandle> EditorViewportRendererVulkan::CreatePipelineState(
        const Render::RenderGraphicsPipelineState &state) {
        using namespace Render;
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = shader_.cooked.metadata.reflection;
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
                                                       shader_.cooked.metadata.cookedArtifactKey,
                                                       reflection.interfaceCompatibility,
                                                       stateKey.Value()};
        const auto pipeline = frontend_.CreateGraphicsPipeline(
            {shader_.vertex, shader_.fragment, EditorVulkanViewportShaderManifest(), target, reflection, state, compatibility});
        if (pipeline.HasError())
            return Result<RenderPipelineHandle>::Failure(pipeline.ErrorValue());
        return Result<RenderPipelineHandle>::Success(pipeline.Value().handle);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateMaterialLayout */
    Result<void> EditorViewportRendererVulkan::CreateMaterialLayout(const Render::NormalizedShaderReflection &reflection) {
        using namespace Render;
        const auto layout = PrepareMaterialBindingLayout(reflection);
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        shader_.layout = layout.Value();
        const auto bytes = ComputeMaterialParameterBufferByteSize(shader_.layout, {1});
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > MaximumUniformBytes)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        shader_.uniformBytes = bytes.Value();
        auto materials = MaterialBindingTable::Create(shader_.pipeline.owner, frontend_,
                                                      {.maximumGenerations = device_.frames * (RenderMaterialPass::MaximumDraws + 1)});
        if (materials.HasError())
            return Result<void>::Failure(materials.ErrorValue());
        shader_.materials = std::move(materials).Value();
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
        using namespace Render;
        const std::array<std::byte, MaximumUniformBytes> zero{};
        const auto buffer = frontend_.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                   {shader_.uniformBytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible},
                                                   std::span{zero}.first(shader_.uniformBytes));
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
            shader_.materials->Publish({.material = {material},
                                        .revision = 1,
                                        .pipeline = pipeline,
                                        .layout = shader_.layout,
                                        .resources = {{{1}, 0, MaterialBufferBinding{draw.buffer, 0, shader_.uniformBytes}}}});
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        const auto acquired = shader_.materials->Acquire(published.Value().binding);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        draw.material = acquired.Value();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
