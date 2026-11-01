// Included inside Horo::Editor after the complete private implementation.
namespace {
    /** @brief Loads the exact packaged native entry without source compilation or retained input borrows. */
    Result<id<MTLFunction>> LoadViewportMetalStage(id<MTLDevice> device, const Render::ShaderArtifactStageView &stage) {
        dispatch_data_t data = dispatch_data_create(stage.bytes.data(), stage.bytes.size(), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = data ? [device newLibraryWithData:data error:&error] : nil;
        NSString *entry =
            [[NSString alloc] initWithBytes:stage.entryPoint.data() length:stage.entryPoint.size() encoding:NSUTF8StringEncoding];
        id<MTLFunction> function = library ? [library newFunctionWithName:entry] : nil;
        const auto expected = stage.stage == Render::ShaderStage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
        if (!function || function.functionType != expected)
            return Result<id<MTLFunction>>::Failure(MakeError(RendererErrors::ViewportMetalPipelineCreationFailed,
                                                              ErrorMessage(@"Rejected exact cooked viewport stage", error)));
        return Result<id<MTLFunction>>::Success(function);
    }

    /** @brief Builds exact mesh/position-only vertex fetch at the slot reserved by the canonical target route. */
    MTLVertexDescriptor *ViewportMetalVertexDescriptor(const std::size_t role) {
        MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
        const std::size_t count = role == 0 ? 3 : 1;
        for (std::size_t index = 0; index < count; ++index) {
            vertex.attributes[index].format = index == 2 ? MTLVertexFormatFloat2 : MTLVertexFormatFloat3;
            vertex.attributes[index].offset = index * sizeof(Math::Vec3);
            vertex.attributes[index].bufferIndex = 30;
        }
        vertex.layouts[30].stride = role == 1 ? sizeof(Math::Vec3) : sizeof(Render::MeshVertex);
        vertex.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
        return vertex;
    }

    Result<id<MTLRenderPipelineState>> CreateViewportReloadPso(id<MTLDevice> device, const Render::CompiledShaderArtifact &artifact,
                                                               const std::size_t role) {
        const auto package =
            Render::DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 64U * 1024U * 1024U);
        if (package.HasError())
            return Result<id<MTLRenderPipelineState>>::Failure(package.ErrorValue());
        if (package.Value().stageCount != 2)
            return Result<id<MTLRenderPipelineState>>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        for (std::size_t index = 0; index < package.Value().stageCount; ++index) {
            const auto &stage = package.Value().stages[index];
            const auto function = LoadViewportMetalStage(device, stage);
            if (function.HasError())
                return Result<id<MTLRenderPipelineState>>::Failure(function.ErrorValue());
            if (stage.stage == Render::ShaderStage::Vertex)
                descriptor.vertexFunction = function.Value();
            else
                descriptor.fragmentFunction = function.Value();
        }
        descriptor.vertexDescriptor = ViewportMetalVertexDescriptor(role);
        if (role != 2)
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *error = nil;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline)
            return Result<id<MTLRenderPipelineState>>::Failure(
                MakeError(RendererErrors::ViewportMetalPipelineCreationFailed,
                          ErrorMessage(@"Cooked viewport pipeline realization failed", error)));
        return Result<id<MTLRenderPipelineState>>::Success(pipeline);
    }

}  // namespace

Result<void> EditorViewportRendererMetal::Impl::CreateReloadPipeline(ProgramGeneration &candidate, const std::size_t role,
                                                                     const Render::ShaderHotReloadProgramView &view) {
    const auto admitted = ViewportReloadArtifact(view, Render::ShaderTargetBackend::Metal, role);
    if (admitted.HasError())
        return Result<void>::Failure(admitted.ErrorValue());
    const auto &artifact = *admitted.Value();
    const auto pipeline = CreateViewportReloadPso(device, artifact, role);
    if (pipeline.HasError())
        return Result<void>::Failure(pipeline.ErrorValue());
    if (const auto layout = ConfigureReloadLayout(candidate, role, artifact.reflection->reflection); layout.HasError())
        return layout;
    if (role == 0)
        candidate.pipeline = pipeline.Value();
    else if (role == 1)
        candidate.gridPipeline = pipeline.Value();
    else
        candidate.shadowPipeline = pipeline.Value();
    return Result<void>::Success();
}

Result<void> EditorViewportRendererMetal::Impl::ConfigureReloadLayout(ProgramGeneration &candidate, const std::size_t role,
                                                                      const Render::NormalizedShaderReflection &reflection) {
    const auto layout = Render::PrepareMaterialBindingLayout(reflection);
    if (layout.HasError())
        return Result<void>::Failure(layout.ErrorValue());
    const auto size = Render::ComputeMaterialParameterBufferByteSize(layout.Value(), {1});
    if (size.HasError())
        return Result<void>::Failure(size.ErrorValue());
    if (size.Value() > 4096)
        return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
    candidate.layouts[role] = layout.Value();
    return ConfigureReloadMappings(candidate, role);
}

Result<void> EditorViewportRendererMetal::Impl::ConfigureReloadMappings(ProgramGeneration &candidate, const std::size_t role) {
    for (const auto &mapping : candidate.layouts[role].targetBindings) {
        if (!mapping.active)
            continue;
        if (mapping.nativeSpace != 0 || mapping.nativeBinding >= 30 || mapping.generatedHelperIndex != 0)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        switch (mapping.id.value) {
            case 1:
                candidate.bufferSlots[role] = mapping.nativeBinding;
                break;
            case 2:
                candidate.textureSlot = mapping.nativeBinding;
                break;
            case 3:
                candidate.samplerSlot = mapping.nativeBinding;
                break;
            default:
                return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        }
    }
    return Result<void>::Success();
}

Result<void> EditorViewportRendererMetal::Impl::UploadReload(id<MTLRenderCommandEncoder> encoder, const std::size_t role,
                                                             const std::span<const std::byte> bytes) const {
    [encoder setVertexBytes:bytes.data() length:bytes.size() atIndex:reloadBufferSlots[role]];
    if (role != 2)
        [encoder setFragmentBytes:bytes.data() length:bytes.size() atIndex:reloadBufferSlots[role]];
    return Result<void>::Success();
}

Result<void> EditorViewportRendererMetal::Impl::UploadReloadGrid(id<MTLRenderCommandEncoder> encoder, const Math::Mat4 &mvp,
                                                                 const Math::Vec3 color) const {
    std::array<std::byte, 4096> bytes{};
    const auto packed = PackEditorViewportGrid(reloadLayouts[1], mvp, color, bytes);
    if (packed.HasError())
        return Result<void>::Failure(packed.ErrorValue());
    return UploadReload(encoder, 1, std::span{bytes}.first(packed.Value()));
}

Result<Render::ShaderHotReloadTargetState> EditorViewportRendererMetal::CaptureState() const {
    using Outcome = Result<Render::ShaderHotReloadTargetState>;
    const auto boundary = Render::Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(*impl_->frontend);
    if (boundary.HasError())
        return Outcome::Failure(boundary.ErrorValue());
    if (!impl_->initialized || !impl_->device || !impl_->graphicsBridge->Device())
        return Outcome::Failure(MakeError(RendererErrors::ViewportNotInitialized));
    return Outcome::Success({Render::ShaderTargetBackend::Metal, boundary.Value().value, impl_->liveGeneration});
}

Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> EditorViewportRendererMetal::Prepare(
    const std::span<const Render::ShaderHotReloadProgramView> programs, const Render::ShaderHotReloadTargetState expected,
    const CancellationToken &cancellation) {
    using Outcome = Result<std::unique_ptr<Render::IShaderHotReloadCandidate>>;
    const auto state = CaptureState();
    if (state.HasError())
        return Outcome::Failure(state.ErrorValue());
    if (expected != state.Value() || programs.size() != 3)
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
    auto candidate = std::make_unique<Impl::ProgramGeneration>();
    candidate->owner = impl_.get();
    candidate->expected = expected;
    candidate->reflected = true;
    for (std::size_t role = 0; role < programs.size(); ++role) {
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
        const auto prepared = impl_->CreateReloadPipeline(*candidate, role, programs[role]);
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
    }
    if (cancellation.IsCancellationRequested())
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
    return Outcome::Success(std::move(candidate));
}

Result<Render::ShaderHotReloadTargetState> EditorViewportRendererMetal::Adopt(std::unique_ptr<Render::IShaderHotReloadCandidate> candidate,
                                                                              const Render::ShaderHotReloadTargetState expected) {
    using Outcome = Result<Render::ShaderHotReloadTargetState>;
    const auto state = CaptureState();
    if (state.HasError())
        return Outcome::Failure(state.ErrorValue());
    auto *generation = dynamic_cast<Impl::ProgramGeneration *>(candidate.get());
    if (!generation || generation->owner != impl_.get() || generation->expected != expected || state.Value() != expected ||
        impl_->liveGeneration == std::numeric_limits<std::uint64_t>::max())
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
    // The selected runtime uses retaining MTLCommandBuffers. Encoded PSOs and setBytes snapshots remain
    // retained by those same commands until their actual completion, independently of these owner swaps.
    impl_->AdoptProgram(*generation);
    ++impl_->liveGeneration;
    return Outcome::Success({expected.backend, expected.deviceIncarnation, impl_->liveGeneration});
}
