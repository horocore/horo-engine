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
        static_cast<void>(view);
        return ValidateEditorViewportMeshReadiness(resources_, pass.scene.instances);
    }

    Result<void> EditorViewportRendererVulkan::PrepareViewportOutput(Render::RenderFrameScope &frame,
                                                                     const Render::PrimaryOutputAttachment &output,
                                                                     const Render::StaticMeshPassDescriptor *viewport) {
        if (viewport) {
            if (auto ready = ValidateEditorViewportMeshReadiness(resources_, viewport->scene.instances); ready.HasError())
                return ready;
        }
        return bridge_.Port()->PreparePrimaryOutput(frame.Token(), output);
    }

    Result<void> EditorViewportRendererVulkan::WriteForwardDraw(Render::FrameToken frame, const DrawSlot &draw,
                                                                const Render::RenderStaticMeshInstance &instance,
                                                                const Render::RenderSceneView &scene) {
        const auto selection = forwardLighting_.SelectDraw(scene, instance, forwardDrawLights_);
        if (selection.HasError())
            return Result<void>::Failure(selection.ErrorValue());
        const auto remapped = forwardLighting_.RemapShadow(preparedShadow_, selection.Value(), scene);
        if (remapped.HasError())
            return Result<void>::Failure(remapped.ErrorValue());
        auto drawScene = scene;
        drawScene.lights = std::span{forwardDrawLights_}.first(selection.Value().count);
        const auto &shadow = remapped.Value();
        return ProgramResources::WriteDraw(bridge_, shader_, resources_.AllocatedExtent(), frame, draw, instance, drawScene,
                                           shadow ? &*shadow : nullptr);
    }
}  // namespace Horo::Editor
