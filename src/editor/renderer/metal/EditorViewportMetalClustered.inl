// Included inside Horo::Editor after the complete private implementation.
namespace {
    /** @brief Adds bounded owner-phase evidence while retaining the exact underlying typed failure. */
    Result<void> ColorFrameFailure(Error error, const char *phase, const EditorViewportColorDiagnostics &state) {
        error.message += std::string{" [viewport phase="} + phase + ", accepted=" + std::to_string(state.acceptedSubmission) +
                         ", generation=" + std::to_string(state.exposureGeneration) + ", candidate=" + std::to_string(state.candidate) +
                         ", reset=" + std::to_string(state.resetPending) + "]";
        return Result<void>::Failure(std::move(error));
    }

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

/** @copydoc EditorViewportRendererMetal::PrepareColorPipeline */
Result<void> EditorViewportRendererMetal::PrepareColorPipeline(const EditorViewportColorInputs &inputs) {
    if (!impl_->initialized || impl_->color || impl_->colorCandidate || impl_->clusteredLighting)
        return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::InvalidInput));
    const EditorViewportColorResourcePort port{impl_.get(), [](void *context, Render::RenderBufferHandle buffer) {
        auto &owner = *static_cast<Impl *>(context);
        return MetalViewportResourceBridge::ColorBufferReadiness(*owner.frontend, buffer);
    }, [](void *context, Render::RenderBufferHandle buffer, std::span<const std::byte> bytes) {
        auto &owner = *static_cast<Impl *>(context);
        const std::array updates{MetalViewportBufferUpdate{buffer, bytes, 0}};
        return MetalViewportResourceBridge::UpdateBuffers(*owner.frontend, *owner.graphicsBridge, updates);
    }};
    impl_->colorCandidate = std::make_unique<EditorViewportColorPipeline>(*impl_->frontend, port);
    if (const auto ready = impl_->colorCandidate->Initialize(inputs); ready.HasError())
        return ready;
    if (const auto resources = impl_->resources.EnableColorPipeline(); resources.HasError())
        return resources;
    impl_->color = std::move(impl_->colorCandidate);
    return Result<void>::Success();
}

/** @copydoc EditorViewportRendererMetal::RequestColorSettings */
Result<void> EditorViewportRendererMetal::RequestColorSettings(const Render::ExposureSettings &settings) {
    if (!impl_->color)
        return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
    return impl_->color->RequestSettings(settings);
}

/** @copydoc EditorViewportRendererMetal::SetColorFrameDelta */
void EditorViewportRendererMetal::SetColorFrameDelta(float deltaSeconds) noexcept {
    impl_->colorDelta = deltaSeconds;
}

/** @copydoc EditorViewportRendererMetal::FinishColorFrame */
void EditorViewportRendererMetal::FinishColorFrame(bool accepted) noexcept {
    if (impl_->color)
        impl_->color->Finish(accepted);
}

/** @copydoc EditorViewportRendererMetal::ColorDiagnostics */
EditorViewportColorDiagnostics EditorViewportRendererMetal::ColorDiagnostics() const noexcept {
    if (impl_->color)
        return impl_->color->Diagnostics();
    return impl_->colorCandidate ? impl_->colorCandidate->Diagnostics() : EditorViewportColorDiagnostics{};
}

Result<void> EditorViewportRendererMetal::PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass,
                                                               const Render::RenderViewId view) {
    if (impl_->color) {
        if (!impl_->nativeLightingActive)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        if (pass.scene.lightSceneDomain != impl_->colorSceneDomain) {
            impl_->color->Reset();
            impl_->colorSceneDomain = pass.scene.lightSceneDomain;
        }
        if (const auto ready = impl_->color->Prepare(view, pass.extent, impl_->colorDelta); ready.HasError())
            return ColorFrameFailure(ready.ErrorValue(), "color-prepare", impl_->color->Diagnostics());
        if (const auto bound = impl_->color->PrepareBindings(impl_->resources.ColorTextureView(), impl_->resources.DisplayTextureView());
            bound.HasError()) {
            const auto failed = ColorFrameFailure(bound.ErrorValue(), "color-bindings", impl_->color->Diagnostics());
            impl_->color->Finish(false);
            return failed;
        }
    }
    if (!impl_->nativeLightingActive)
        return ValidateEditorViewportMeshReadiness(impl_->resources, pass.scene.instances);
    if (view != LightingViewIdentity())
        return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
    impl_->clusteredFrame->SetPresentation(impl_->gridOptions, impl_->lightVisualizerOptions);
    const auto prepared = impl_->clusteredFrame->Prepare(pass, view);
    if (prepared.HasError())
        return ColorFrameFailure(prepared.ErrorValue(), "scene-prepare", ColorDiagnostics());
    return prepared;
}

Result<void> EditorViewportRendererMetal::ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                               const std::span<const Render::RenderPassDescriptor> passes) {
    if (!impl_->nativeLightingActive) {
        if (impl_->color)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        return frame.Execute(passes);
    }
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
    if (encoded.HasError()) {
        frame.Cancel();
        return ColorFrameFailure(encoded.ErrorValue(), "graph-execute", ColorDiagnostics());
    }
    return encoded;
}

Result<void> EditorViewportRendererMetal::Impl::PrepareClusteredReload(ProgramGeneration &candidate,
                                                                       const std::span<const Render::ShaderHotReloadProgramView> programs) {
    candidate.clusteredLighting = std::make_unique<MetalViewportClusteredLighting>(*frontend, color != nullptr);
    if (const auto prepared = candidate.clusteredLighting->PrepareReplacement(programs, *clusteredLighting, clusteredProfile);
        prepared.HasError())
        return prepared;
    candidate.clusteredFrame = std::make_unique<MetalViewportClusteredFrame>(*frontend, *candidate.clusteredLighting, resources,
                                                                             *graphicsBridge, clusteredProfile, color.get());
    if (!candidate.clusteredLighting->UsesCulling())
        return candidate.clusteredFrame->ConfigureBaseline(clusteredFrame->Diagnostics().selection);
    return Result<void>::Success();
}
