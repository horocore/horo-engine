// Included inside Horo::Editor after native compile helpers and the candidate owner.
namespace {
    struct ViewportGLStages final {
        std::array<GLuint, 2> shaders{};

        ~ViewportGLStages() {
            for (const auto shader : shaders)
                if (shader)
                    glDeleteShader(shader);
        }
    };

    /** @brief Compiles and links exact translated bytes on the selected owner context. */
    Result<GLuint> LinkViewportReload(const Render::CompiledShaderArtifact &artifact) {
        const auto package =
            Render::DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 64U * 1024U * 1024U);
        if (package.HasError())
            return Result<GLuint>::Failure(package.ErrorValue());
        if (package.Value().stageCount != 2)
            return Result<GLuint>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        ViewportGLStages owner;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto &stage = package.Value().stages[index];
            const std::string source(stage.bytes.begin(), stage.bytes.end());
            const auto shader =
                CompileShader(stage.stage == Render::ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, source.c_str());
            if (shader.HasError())
                return Result<GLuint>::Failure(shader.ErrorValue());
            owner.shaders[index] = shader.Value();
        }
        const GLuint program = glCreateProgram();
        for (const auto shader : owner.shaders)
            glAttachShader(program, shader);
        glLinkProgram(program);
        GLint linked{};
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            std::array<char, 4096> log{};
            glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            glDeleteProgram(program);
            return Result<GLuint>::Failure(MakeError(RendererErrors::ViewportShaderLinkFailed, std::string(log.data())));
        }
        return Result<GLuint>::Success(program);
    }

    Result<GLint> ConfigureViewportGlUniform(const GLuint program, const Render::MaterialBindingLayout &layout) {
        GLint nativeBytes{};
        for (const auto &mapping : layout.targetBindings) {
            if (!mapping.active || mapping.id.value != 1)
                continue;
            const auto block = glGetUniformBlockIndex(program, mapping.nativeName.c_str());
            if (block == GL_INVALID_INDEX)
                return Result<GLint>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &nativeBytes);
            glUniformBlockBinding(program, block, mapping.nativeBinding);
        }
        return Result<GLint>::Success(nativeBytes);
    }

    Result<std::uint32_t> ConfigureViewportGlSampler(const GLuint program, const Render::MaterialBindingLayout &layout) {
        std::uint32_t unit{};
        for (const auto &mapping : layout.targetBindings) {
            if (!mapping.active || mapping.id.value != 2)
                continue;
            const auto location = glGetUniformLocation(program, mapping.nativeName.c_str());
            if (location < 0)
                return Result<std::uint32_t>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
            unit = mapping.nativeBinding;
        }
        return Result<std::uint32_t>::Success(unit);
    }
}  // namespace

Result<void> EditorViewportRendererOpenGL::CreateReloadProgram(ProgramGeneration &candidate, const std::size_t role,
                                                               const Render::ShaderHotReloadProgramView &view) {
    const auto artifact = ViewportReloadArtifact(view, Render::ShaderTargetBackend::OpenGL, role);
    if (artifact.HasError())
        return Result<void>::Failure(artifact.ErrorValue());
    const auto linked = LinkViewportReload(*artifact.Value());
    if (linked.HasError())
        return Result<void>::Failure(linked.ErrorValue());
    const auto program = linked.Value();
    if (role == 0)
        candidate.program = program;
    else if (role == 1)
        candidate.gridProgram = program;
    else
        candidate.shadowProgram = program;
    const auto layout = Render::PrepareMaterialBindingLayout(artifact.Value()->reflection->reflection);
    if (layout.HasError())
        return Result<void>::Failure(layout.ErrorValue());
    const auto valid = Render::Detail::ValidateOpenGLProgramInterface(program, *view.manifest, view.manifest->targets.front(),
                                                                      artifact.Value()->reflection->reflection);
    if (valid.HasError())
        return valid;
    candidate.layouts[role] = layout.Value();
    for (const auto &mapping : layout.Value().targetBindings)
        if (mapping.active && mapping.id.value == 1)
            candidate.bufferSlots[role] = mapping.nativeBinding;
    return CreateReloadUniformBuffer(candidate, role, program);
}

Result<void> EditorViewportRendererOpenGL::CreateReloadUniformBuffer(ProgramGeneration &candidate, const std::size_t role,
                                                                     const std::uint32_t program) {
    const auto native = ConfigureViewportGlUniform(program, candidate.layouts[role]);
    if (native.HasError())
        return Result<void>::Failure(native.ErrorValue());
    const auto nativeBytes = native.Value();
    const auto bytes = Render::ComputeMaterialParameterBufferByteSize(candidate.layouts[role], {1});
    if (bytes.HasError())
        return Result<void>::Failure(bytes.ErrorValue());
    if (nativeBytes <= 0 || nativeBytes > 4096 || bytes.Value() > static_cast<std::size_t>(nativeBytes))
        return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
    candidate.bufferBytes[role] = static_cast<std::size_t>(nativeBytes);
    glGenBuffers(1, &candidate.buffers[role]);
    glBindBuffer(GL_UNIFORM_BUFFER, candidate.buffers[role]);
    glBufferData(GL_UNIFORM_BUFFER, nativeBytes, nullptr, GL_DYNAMIC_DRAW);
    if (glGetError() != GL_NO_ERROR)
        return Result<void>::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
    return Result<void>::Success();
}

Result<void> EditorViewportRendererOpenGL::UploadReload(const std::size_t role, const std::span<const std::byte> bytes) const {
    if (bytes.size() > reloadBufferBytes_[role])
        return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
    glBindBuffer(GL_UNIFORM_BUFFER, reloadBuffers_[role]);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(bytes.size()), bytes.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, reloadBufferSlots_[role], reloadBuffers_[role]);
    return Result<void>::Success();
}

Result<void> EditorViewportRendererOpenGL::UploadReloadGrid(const Math::Mat4 &mvp, const Math::Vec3 color) const {
    std::array<std::byte, 4096> bytes{};
    const auto packed = PackEditorViewportGrid(reloadLayouts_[1], mvp, color, bytes);
    if (packed.HasError())
        return Result<void>::Failure(packed.ErrorValue());
    return UploadReload(1, std::span{bytes}.first(packed.Value()));
}

Result<Render::ShaderHotReloadTargetState> EditorViewportRendererOpenGL::CaptureState() const {
    using Outcome = Result<Render::ShaderHotReloadTargetState>;
    const auto boundary = Render::Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(*frontend_);
    if (boundary.HasError())
        return Outcome::Failure(boundary.ErrorValue());
    if (!initialized_ || program_ == 0 || !glGetString(GL_VERSION))
        return Outcome::Failure(MakeError(RendererErrors::ViewportNotInitialized));
    return Outcome::Success({Render::ShaderTargetBackend::OpenGL, boundary.Value().value, liveGeneration_});
}

Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> EditorViewportRendererOpenGL::Prepare(
    const std::span<const Render::ShaderHotReloadProgramView> programs, const Render::ShaderHotReloadTargetState expected,
    const CancellationToken &cancellation) {
    using Outcome = Result<std::unique_ptr<Render::IShaderHotReloadCandidate>>;
    const auto state = CaptureState();
    if (state.HasError())
        return Outcome::Failure(state.ErrorValue());
    if (expected != state.Value() || programs.size() != 3)
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
    OpenGLStateSnapshot restore;
    auto candidate = std::make_unique<ProgramGeneration>();
    candidate->owner = this;
    candidate->expected = expected;
    candidate->reflected = true;
    for (std::size_t role = 0; role < programs.size(); ++role) {
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
        const auto prepared = CreateReloadProgram(*candidate, role, programs[role]);
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
    }
    const auto sampler = ConfigureViewportGlSampler(candidate->program, candidate->layouts[0]);
    if (sampler.HasError())
        return Outcome::Failure(sampler.ErrorValue());
    candidate->textureUnit = sampler.Value();
    if (cancellation.IsCancellationRequested())
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
    return Outcome::Success(std::move(candidate));
}

Result<Render::ShaderHotReloadTargetState> EditorViewportRendererOpenGL::Adopt(std::unique_ptr<Render::IShaderHotReloadCandidate> candidate,
                                                                               const Render::ShaderHotReloadTargetState expected) {
    using Outcome = Result<Render::ShaderHotReloadTargetState>;
    const auto state = CaptureState();
    if (state.HasError())
        return Outcome::Failure(state.ErrorValue());
    auto *generation = dynamic_cast<ProgramGeneration *>(candidate.get());
    if (!generation || generation->owner != this || generation->expected != expected || expected != state.Value() ||
        liveGeneration_ == std::numeric_limits<std::uint64_t>::max())
        return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
    // GL retains all objects referenced by queued rendering; deletion only removes this owner's name.
    // Previous draws retain their actual driver-owned objects and buffer contents until execution completes.
    AdoptProgram(*generation);
    ++liveGeneration_;
    return Outcome::Success({expected.backend, expected.deviceIncarnation, liveGeneration_});
}
