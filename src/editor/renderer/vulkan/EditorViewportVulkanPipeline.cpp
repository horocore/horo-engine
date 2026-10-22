#include "EditorViewportRendererVulkan.h"
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
        const auto device = frontend_.QueryPipelineDeviceIdentity();
        if (device.HasError())
            return Result<void>::Failure(device.ErrorValue());
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .depthFormat = RenderTextureFormat::Depth32Float};
        const auto stateKey = ComputeRenderGraphicsPipelineStateDigest(state);
        if (stateKey.HasError())
            return Result<void>::Failure(stateKey.ErrorValue());
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
            return Result<void>::Failure(pipeline.ErrorValue());
        shader_.pipeline = pipeline.Value().handle;
        return CreateMaterialLayout(reflection);
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
        auto materials = MaterialBindingTable::Create(shader_.pipeline.owner, frontend_);
        if (materials.HasError())
            return Result<void>::Failure(materials.ErrorValue());
        shader_.materials = std::move(materials).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::CreateDrawSlots */
    Result<void> EditorViewportRendererVulkan::CreateDrawSlots() {
        using namespace Render;
        const std::array<std::byte, MaximumUniformBytes> zero{};
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::size_t index = 0; index < RenderMaterialPass::MaximumDraws; ++index) {
                auto &draw = slots_[slot].draws[index];
                const auto buffer =
                    frontend_.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                           {shader_.uniformBytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible},
                                           std::span{zero}.first(shader_.uniformBytes));
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                draw.buffer = buffer.Value().handle;
                if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
                    return processed;
                if (const auto ready = frontend_.ResourceOperationResult(buffer.Value().operation); ready.HasError())
                    return ready;
                const auto native = frontend_.BackendInstance(draw.buffer);
                if (native.HasError())
                    return Result<void>::Failure(native.ErrorValue());
                draw.instance = native.Value();
                const auto published =
                    shader_.materials->Publish({.material = {slot * RenderMaterialPass::MaximumDraws + index + 1},
                                                .revision = 1,
                                                .pipeline = shader_.pipeline,
                                                .layout = shader_.layout,
                                                .resources = {{{1}, 0, MaterialBufferBinding{draw.buffer, 0, shader_.uniformBytes}}}});
                if (published.HasError())
                    return Result<void>::Failure(published.ErrorValue());
                const auto acquired = shader_.materials->Acquire(published.Value().binding);
                if (acquired.HasError())
                    return Result<void>::Failure(acquired.ErrorValue());
                draw.material = acquired.Value();
            }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
