#include "EditorViewportBaselineDrawParameters.h"

#include <array>

namespace Horo::Editor {
    /** @copydoc PrepareEditorViewportBaselineDrawParameters */
    Result<void> PrepareEditorViewportBaselineDrawParameters(const EditorViewportForwardLighting &lighting,
                                                             const Render::RenderSceneView &scene,
                                                             const Render::RenderStaticMeshInstance &instance,
                                                             const std::optional<Render::DirectionalShadowFrame> &shadow,
                                                             Render::ShaderTargetBackend target,
                                                             EditorViewportShaderParameters &destination) {
        std::array<Render::RenderLight, Render::MaximumForwardLights> lights;
        const auto selection = lighting.SelectDraw(scene, instance, lights);
        if (selection.HasError())
            return Result<void>::Failure(selection.ErrorValue());
        const auto localShadow = lighting.RemapShadow(shadow, selection.Value(), scene);
        if (localShadow.HasError())
            return Result<void>::Failure(localShadow.ErrorValue());
        auto localScene = scene;
        localScene.lights = std::span{lights}.first(selection.Value().count);
        destination.SetScene(localScene, localShadow.Value(), target);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
