#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>

namespace Horo::Editor {
    Result<void> EditorViewportRendererVulkan::RequestDirectionalShadowProfile(const Render::DirectionalShadowProfile &profile) {
        return resources_.RequestDirectionalShadowProfile(profile);
    }

    Result<void> EditorViewportRendererVulkan::PrepareShadowProgram(ShaderResources &program, std::span<FrameSlot> slots) {
        using namespace Render;
        const auto encoded = EncodeReflectedShaderArtifact(program.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (auto checked = LoadEditorVulkanShadowCook(program.payload, encoded.Value()); checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        program.depthOnly = true;
        if (auto stages = ProgramResources::CreateProgramStages(frontend_, program); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                .depthFormat = RenderTextureFormat::Depth32Float,
                                                .depthOnly = true};
        auto pipeline = ProgramResources::CreatePipelineState(frontend_, program, EditorVulkanShadowShaderManifest(), state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        program.pipeline = pipeline.Value();
        if (auto layout = ProgramResources::CreateMaterialLayout(frontend_, program, program.metadata.reflection, device_.frames);
            layout.HasError())
            return layout;
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::uint32_t cascade = 0; cascade < MaximumDirectionalShadowCascades; ++cascade)
                for (std::size_t draw = 0; draw < RenderMaterialPass::MaximumDraws; ++draw) {
                    const auto identity = (slot * MaximumDirectionalShadowCascades + cascade) * RenderMaterialPass::MaximumDraws + draw + 1;
                    if (auto created = ProgramResources::CreateUniformSlot(frontend_, program, slots[slot].shadows[cascade][draw], identity,
                                                                           program.pipeline);
                        created.HasError())
                        return created;
                }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererVulkan::BindShadowAtlas() {
        const auto atlas = resources_.ShadowTextureView();
        if (boundShadowAtlas_ == atlas)
            return Result<void>::Success();
        if (!atlas.IsValid())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        for (std::uint32_t frame = 0; frame < device_.frames; ++frame) {
            for (auto &draw : slots_[frame].draws)
                if (auto bound = ProgramResources::BindUniformSlot(shader_, draw, shader_.pipeline, atlas); bound.HasError())
                    return bound;
            if (auto bound = ProgramResources::BindUniformSlot(shader_, slots_[frame].overlay.uniforms, shader_.overlayPipeline, atlas);
                bound.HasError())
                return bound;
        }
        boundShadowAtlas_ = atlas;
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererVulkan::WriteShadowDraw(Render::FrameToken frame, const DrawSlot &draw,
                                                               const Render::RenderStaticMeshInstance &instance,
                                                               const Render::DirectionalShadowFrame &shadow, std::uint32_t cascade) {
        auto relativeModel = instance.localToWorld;
        relativeModel.values[12] -= shadow.cameraOrigin.x;
        relativeModel.values[13] -= shadow.cameraOrigin.y;
        relativeModel.values[14] -= shadow.cameraOrigin.z;
        auto mvp = Math::Multiply(shadow.cascades[cascade].cameraRelativeViewProjection, relativeModel);
        for (std::size_t column = 0; column < 4; ++column)
            mvp.values[column * 4 + 1] *= -1;
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(shadowShader_.uniformBytes);
        Math::Mat4 vectorMvp;
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                vectorMvp.values[column * 4 + row] = mvp.values[row * 4 + column];
        // The shared depth HLSL uses mul(vector,row_majorMatrix); reflected packing receives its logical transpose.
        if (auto written = ProgramResources::WriteParameter(shadowShader_.layout, bytes, {1}, vectorMvp.values); written.HasError())
            return written;
        return bridge_.Port()->WriteFrameBuffer(frame, draw.instance, bytes);
    }

    Result<void> EditorViewportRendererVulkan::PrepareShadowDraws(Render::FrameToken frame, std::uint32_t slot,
                                                                  const Render::RenderSceneView &scene,
                                                                  const Render::DirectionalShadowFrame &shadow) {
        for (std::uint32_t cascade = 0; cascade < shadow.cascadeCount; ++cascade)
            for (std::size_t index = 0; index < scene.instances.size(); ++index) {
                const auto &instance = scene.instances[index];
                const auto mesh = resources_.FindMesh(instance.mesh.id.value);
                if (!mesh)
                    return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
                const auto &draw = slots_[slot].shadows[cascade][index];
                if (auto written = WriteShadowDraw(frame, draw, instance, shadow, cascade); written.HasError())
                    return written;
                preparedShadowDraws_[cascade][index] = {mesh->mesh, draw.material};
            }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererVulkan::ExecuteShadowedViewport(Render::RenderFrameScope &frame,
                                                                       const Render::StaticMeshPassDescriptor &pass, std::size_t draws,
                                                                       const Render::DirectionalShadowFrame *shadow) {
        using namespace Render;
        std::array<RenderMaterialPass, MaximumDirectionalShadowCascades> preceding{};
        const auto count = shadow ? shadow->cascadeCount : 1;
        const auto extent = resources_.ShadowExtent();
        for (std::uint32_t cascade = 0; cascade < count; ++cascade) {
            auto &depth = preceding[cascade];
            depth.target = resources_.ShadowTarget();
            depth.extent = {extent.width, extent.height};
            depth.depthOnly = true;
            depth.depthClear = cascade == 0 ? std::optional{1.0F} : std::nullopt;
            if (shadow) {
                const auto &viewport = shadow->cascades[cascade].viewport;
                depth.region = RenderMaterialRegion{viewport.x, viewport.y, viewport.width, viewport.height};
                depth.draws = std::span{preparedShadowDraws_[cascade]}.first(pass.scene.instances.size());
            }
        }
        const auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return Result<void>::Failure(reserved.ErrorValue());
        recordedEver_ = true;
        return frame.ExecuteMaterialPass({.target = pass.target,
                                          .extent = pass.extent,
                                          .color = {.clearColor = pass.clearColor},
                                          .depthClear = 1.0F,
                                          .draws = std::span{preparedDraws_}.first(draws),
                                          .submission = reserved.Value(),
                                          .precedingPasses = std::span{preceding}.first(count)});
    }
}  // namespace Horo::Editor
