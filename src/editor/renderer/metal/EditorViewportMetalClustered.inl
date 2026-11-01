// Included inside Horo::Editor after the complete private implementation.
namespace {
    struct ClusteredViewportPasses final {
        const Render::StaticMeshPassDescriptor *viewport{};
        const Render::PrimaryOutputAttachment *output{};
    };

    Result<ClusteredViewportPasses> FindClusteredViewportPasses(const std::span<const Render::RenderPassDescriptor> passes) {
        using Outcome = Result<ClusteredViewportPasses>;
        if (passes.size() > 2)
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        ClusteredViewportPasses result;
        for (const auto &pass : passes) {
            if (pass.kind != Render::RenderPassKind::Graphics || pass.staticMesh.has_value() == pass.primaryOutput.has_value())
                return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            if (pass.staticMesh) {
                if (result.viewport)
                    return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
                result.viewport = &*pass.staticMesh;
            } else {
                if (result.output)
                    return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
                result.output = &*pass.primaryOutput;
            }
        }
        return Outcome::Success(result);
    }
}  // namespace

Result<void> EditorViewportRendererMetal::PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass,
                                                               const Render::RenderViewId view) {
    if (!impl_->nativeLightingActive)
        return ValidateEditorViewportMeshReadiness(impl_->resources, pass.scene.instances);
    if (view != LightingViewIdentity())
        return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
    impl_->clusteredFrame->SetPresentation(impl_->gridOptions, impl_->lightVisualizerOptions);
    return impl_->clusteredFrame->Prepare(pass, view);
}

Result<void> EditorViewportRendererMetal::ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                               const std::span<const Render::RenderPassDescriptor> passes) {
    if (!impl_->nativeLightingActive)
        return frame.Execute(passes);
    const auto parsed = FindClusteredViewportPasses(passes);
    if (parsed.HasError()) {
        frame.Cancel();
        return Result<void>::Failure(parsed.ErrorValue());
    }
    if (!parsed.Value().viewport)
        return frame.Execute(passes);
    if (!parsed.Value().output) {
        frame.Cancel();
        return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
    }
    const auto encoded = impl_->clusteredFrame->Execute(frame, *parsed.Value().viewport, *parsed.Value().output);
    if (encoded.HasError())
        frame.Cancel();
    return encoded;
}

Result<void> EditorViewportRendererMetal::Impl::PrepareClusteredReload(ProgramGeneration &candidate,
                                                                       const std::span<const Render::ShaderHotReloadProgramView> programs) {
    candidate.clusteredLighting = std::make_unique<MetalViewportClusteredLighting>(*frontend);
    if (const auto prepared = candidate.clusteredLighting->PrepareReplacement(programs, *clusteredLighting, clusteredProfile);
        prepared.HasError())
        return prepared;
    candidate.clusteredFrame = std::make_unique<MetalViewportClusteredFrame>(*frontend, *candidate.clusteredLighting, resources,
                                                                             *graphicsBridge, clusteredProfile);
    return Result<void>::Success();
}
