// Included inside Horo::Editor; the shared owner consumes only the explicit host-selected CPU recipe.
Result<void> EditorViewportRendererOpenGL::RequestLightingProfile(const EditorViewportLightingProfile &profile,
                                                                  const Render::LightPreparationSelection &selection) {
    return forwardLighting_.Configure(profile, selection);
}

EditorViewportLightingDiagnostics EditorViewportRendererOpenGL::LightingDiagnostics() const noexcept {
    return forwardLighting_.Diagnostics();
}

/** @copydoc EditorViewportRendererOpenGL::PrepareLightingFrame */
Result<void> EditorViewportRendererOpenGL::PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass,
                                                                const Render::RenderViewId view) {
    static_cast<void>(view);
    return ValidateEditorViewportMeshReadiness(resources_, pass.scene.instances);
}

Result<void> EditorViewportRendererOpenGL::PrepareDrawLighting(const Render::RenderSceneView &scene,
                                                               const Render::RenderStaticMeshInstance &instance) {
    std::array<Render::RenderLight, Render::MaximumForwardLights> lights;
    const auto selection = forwardLighting_.SelectDraw(scene, instance, lights);
    if (selection.HasError())
        return Result<void>::Failure(selection.ErrorValue());
    const auto shadow = forwardLighting_.RemapShadow(frameShadow_, selection.Value(), scene);
    if (shadow.HasError())
        return Result<void>::Failure(shadow.ErrorValue());
    auto localScene = scene;
    localScene.lights = std::span{lights}.first(selection.Value().count);
    reloadParameters_.SetScene(localScene, shadow.Value(), Render::ShaderTargetBackend::OpenGL);
    if (!reflectedProgram_)
        UploadLighting(localScene, shadow.Value());
    return Result<void>::Success();
}
