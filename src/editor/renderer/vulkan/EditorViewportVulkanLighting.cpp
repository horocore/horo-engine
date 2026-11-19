#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorViewportMeshReadiness.h"

namespace Horo::Editor {
    Result<void> EditorViewportRendererVulkan::RequestLightingProfile(const EditorViewportLightingProfile &profile,
                                                                      const Render::LightPreparationSelection &selection) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        return forwardLighting_.Configure(profile, selection);
    }

    Result<void> EditorViewportRendererVulkan::PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass,
                                                                    const Render::RenderViewId view) {
        if (auto ready = ValidateEditorViewportMeshReadiness(resources_, pass.scene.instances); ready.HasError())
            return ready;
        if (!frontend_.Capabilities().supportsMaterialRasterGraph)
            return Result<void>::Success();
        if (view != LightingViewIdentity())
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        if (color_)
            if (const auto ready = PrepareViewportColor(view, pass.extent); ready.HasError())
                return ready;
        const auto prepared = PrepareViewportRasterGraphs(pass);
        if (prepared.HasError() && color_)
            FinishColorFrame(false);
        return prepared;
    }

    Result<void> EditorViewportRendererVulkan::PrepareViewportOutput(Render::RenderFrameScope &frame,
                                                                     const Render::PrimaryOutputAttachment &output,
                                                                     const Render::StaticMeshPassDescriptor *viewport) const {
        if (viewport) {
            if (auto ready = ValidateEditorViewportMeshReadiness(resources_, viewport->scene.instances); ready.HasError())
                return ready;
        }
        return bridge_.Port()->PreparePrimaryOutput(frame.Token(), output);
    }

    Result<void> EditorViewportRendererVulkan::WriteForwardDraw(Render::FrameToken frame, const DrawSlot &draw,
                                                                const Render::RenderStaticMeshInstance &instance,
                                                                const Render::RenderSceneView &scene,
                                                                const std::optional<Render::DirectionalShadowFrame> &shadow) {
        const auto selection = forwardLighting_.SelectDraw(scene, instance, forwardDrawLights_);
        if (selection.HasError())
            return Result<void>::Failure(selection.ErrorValue());
        const auto remapped = forwardLighting_.RemapShadow(shadow, selection.Value(), scene);
        if (remapped.HasError())
            return Result<void>::Failure(remapped.ErrorValue());
        auto drawScene = scene;
        drawScene.lights = std::span{forwardDrawLights_}.first(selection.Value().count);
        const auto &drawShadow = remapped.Value();
        return ProgramResources::WriteDraw(bridge_, SceneProgram(), resources_.AllocatedExtent(), frame, draw, instance, drawScene,
                                           drawShadow ? &*drawShadow : nullptr);
    }
}  // namespace Horo::Editor
