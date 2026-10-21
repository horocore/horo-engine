/** @brief Resource realization methods kept with OpenGLRenderBackend's private definition. */
/** @copydoc IMaterialBindingBackend::QueryMaterialParameterBufferSize */
Result<std::size_t> QueryMaterialParameterBufferSize(const std::uint64_t pipeline, const MaterialBindingDescriptor &descriptor,
                                                     const ShaderBindingId binding) const override {
    if (!IsOwnerThread())
        return WrongThread<std::size_t>();
    if (!initialized_ || frameActive_ || !functions_.materials.IsValid())
        return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    if (pipeline > std::numeric_limits<std::uint32_t>::max())
        return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    const auto program = prepared_.graphicsPipelines.find(static_cast<std::uint32_t>(pipeline));
    if (program == prepared_.graphicsPipelines.end() || !Detail::SameMaterialLayout(program->second.layout, descriptor.layout))
        return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    const auto &layout = program->second.layout;
    const auto resource = std::ranges::find(layout.resources, binding, &ShaderReflectedBinding::id);
    const auto mapping = std::ranges::find_if(layout.targetBindings, [&](const auto &entry) {
        return entry.id == binding && entry.active && entry.generatedHelperIndex == 0;
    });
    if (resource == layout.resources.end() || resource->kind != ShaderResourceKind::UniformBuffer || mapping == layout.targetBindings.end())
        return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    return functions_.materials.uniformBlockSize(program->first, mapping->nativeName);
}

/** @copydoc IMaterialBindingBackend::RealizeResolved */
Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &descriptor, const std::uint64_t pipeline,
                                                                  const std::span<const MaterialResidentResource> resources) override {
    using Realization = Result<std::unique_ptr<IResidentMaterialBinding>>;
    if (!IsOwnerThread())
        return WrongThread<std::unique_ptr<IResidentMaterialBinding>>();
    if (!initialized_ || frameActive_ || !functions_.materials.IsValid())
        return Realization::Failure(MakeError(MaterialBindingErrors::Unsupported));
    const auto valid = ValidateMaterialBindingDescriptor(descriptor, descriptor.pipeline.owner);
    if (valid.HasError())
        return Realization::Failure(valid.ErrorValue());
    if (pipeline > std::numeric_limits<std::uint32_t>::max() || resources.size() != descriptor.resources.size())
        return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    const auto program = prepared_.graphicsPipelines.find(static_cast<std::uint32_t>(pipeline));
    if (program == prepared_.graphicsPipelines.end() || !Detail::SameMaterialLayout(program->second.layout, descriptor.layout))
        return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                                              "Material layout does not match the exact resident program's cooked layout."));
    const auto alignment = functions_.materials.uniformOffsetAlignment();
    if (alignment.HasError())
        return Realization::Failure(alignment.ErrorValue());
    if (alignment.Value() == 0)
        return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    auto material = std::make_unique<Detail::OpenGLMaterialBinding>();
    material->owner = this;
    material->program = program->first;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto &logical = descriptor.resources[index];
        const auto &resident = resources[index];
        const auto mapping = std::ranges::find_if(descriptor.layout.targetBindings, [&](const auto &entry) {
            return entry.id == logical.binding && entry.active && entry.generatedHelperIndex == 0;
        });
        const auto declaration = std::ranges::find(descriptor.layout.resources, logical.binding, &ShaderReflectedBinding::id);
        if (mapping == descriptor.layout.targetBindings.end() || declaration == descriptor.layout.resources.end() ||
            logical.arrayElement != 0 || declaration->arrayCount != 1 || mapping->nativeSpace != 0 || resident.instance == 0)
            return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
        Detail::OpenGLMaterialSlot slot{.kind = resident.kind,
                                        .binding = mapping->nativeBinding,
                                        .offset = resident.offset,
                                        .byteCount = resident.byteCount};
        if (declaration->kind == ShaderResourceKind::UniformBuffer) {
            if (resident.kind != MaterialResidentResourceKind::Buffer || resident.instance > std::numeric_limits<std::uint32_t>::max())
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto buffer = bufferDescriptors_.find(static_cast<std::uint32_t>(resident.instance));
            const auto size = QueryMaterialParameterBufferSize(pipeline, descriptor, logical.binding);
            if (size.HasError())
                return Realization::Failure(size.ErrorValue());
            if (buffer == bufferDescriptors_.end() || !HasBufferUsage(buffer->second.usage, RenderBufferUsage::Uniform) ||
                resident.offset % alignment.Value() != 0 || resident.offset > buffer->second.byteSize ||
                resident.byteCount > buffer->second.byteSize - resident.offset || resident.byteCount < size.Value())
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            slot.object = buffer->first;
        } else if (declaration->kind == ShaderResourceKind::SampledTexture) {
            const auto view = textureViews_.find(resident.instance);
            if (resident.kind != MaterialResidentResourceKind::TextureView || view == textureViews_.end() ||
                !HasTextureUsage(view->second.usage, RenderTextureUsage::Sampled) || view->second.sampleCount != 1)
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto paired = std::ranges::find(descriptor.resources, mapping->pairedSampler, &MaterialResourceBinding::binding);
            if (paired == descriptor.resources.end())
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto &samplerInstance = resources[static_cast<std::size_t>(paired - descriptor.resources.begin())];
            if (samplerInstance.kind != MaterialResidentResourceKind::Sampler ||
                samplerInstance.instance > std::numeric_limits<std::uint32_t>::max())
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto sampler = prepared_.samplerDescriptors.find(static_cast<std::uint32_t>(samplerInstance.instance));
            const auto comparison = functions_.materials.textureComparison(program->first, mapping->nativeName);
            if (comparison.HasError())
                return Realization::Failure(comparison.ErrorValue());
            if (sampler == prepared_.samplerDescriptors.end() || sampler->second.compareEnabled != comparison.Value() ||
                (comparison.Value() && view->second.aspect != RenderTextureAspect::Depth))
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            slot.object = view->second.texture;
            slot.sampler = sampler->first;
        } else if (declaration->kind == ShaderResourceKind::Sampler) {
            if (resident.kind != MaterialResidentResourceKind::Sampler || resident.instance > std::numeric_limits<std::uint32_t>::max() ||
                !prepared_.samplerDescriptors.contains(static_cast<std::uint32_t>(resident.instance)))
                return Realization::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            continue;  // Separate sampler policy is applied only through its cooked sampled-texture pairing.
        } else {
            return Realization::Failure(MakeError(MaterialBindingErrors::Unsupported));
        }
        material->slots[material->count++] = slot;
    }
    return Realization::Success(std::move(material));
}

/** @copydoc IRenderResourceBackend::QueryPipelineDeviceIdentity */
Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const override {
    if (!IsOwnerThread())
        return WrongThread<RenderPipelineDeviceIdentity>();
    if (!initialized_)
        return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::NotInitialized));
    if (frameActive_)
        return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::FrameActive));
    if (!functions_.pipelines.queryDeviceIdentity)
        return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    return functions_.pipelines.queryDeviceIdentity();
}

/** @copydoc IRenderResourceBackend::QueryGraphicsPipelineMemoryCost */
Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &descriptor, const std::uint64_t vertex,
                                                             const std::uint64_t fragment) const override {
    if (!IsOwnerThread())
        return WrongThread<RenderMemoryCostPlan>();
    const auto valid = ValidateRenderGraphicsPipelineDescriptor(descriptor);
    if (valid.HasError())
        return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
    if (!initialized_ || !functions_.shaders.IsValid() || !functions_.pipelines.IsValid() || !functions_.pipelines.isAvailable() ||
        !functions_.materialExecution.IsValid() || !functions_.materialExecution.isAvailable() || vertex == 0 || fragment == 0 ||
        vertex == fragment || vertex > std::numeric_limits<std::uint32_t>::max() || fragment > std::numeric_limits<std::uint32_t>::max())
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    const auto vertexModule = prepared_.shaderModules.find(static_cast<std::uint32_t>(vertex));
    const auto fragmentModule = prepared_.shaderModules.find(static_cast<std::uint32_t>(fragment));
    const auto matches = [&](const auto &module, const ShaderStage stage) {
        return module != prepared_.shaderModules.end() && module->second.descriptor.stage == stage &&
               Detail::SameShaderTarget(module->second.descriptor.target, descriptor.target) &&
               module->second.descriptor.artifactKey == descriptor.compatibility.shaderArtifactKey &&
               module->second.descriptor.shaderInterface == descriptor.reflection.interfaceCompatibility &&
               std::ranges::any_of(descriptor.manifest.entryPoints, [&](const auto &entry) {
            return entry.stage == stage && entry.name == module->second.descriptor.entryPoint;
        });
    };
    if (!matches(vertexModule, ShaderStage::Vertex) || !matches(fragmentModule, ShaderStage::Fragment))
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                      "Graphics pipeline references stale native modules or a different cooked artifact/interface/target."));
    if (descriptor.state.viewRouting != RenderViewRouting::SingleView || descriptor.state.sampleCount != 1 ||
        descriptor.state.colorFormat != RenderTextureFormat::Rgba8Unorm ||
        (descriptor.state.depthFormat && *descriptor.state.depthFormat != RenderTextureFormat::Depth24Stencil8 &&
         *descriptor.state.depthFormat != RenderTextureFormat::Depth32Float) ||
        !descriptor.manifest.inlineConstants.empty() || !descriptor.manifest.specializationInputs.empty())
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    if (std::ranges::any_of(descriptor.reflection.targetBindings, [](const auto &mapping) {
        return mapping.active && mapping.nativeBinding >= 128;
    }))
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL material consumption has a fixed 128-slot resource state restoration envelope."));
    const auto identity = QueryPipelineDeviceIdentity();
    if (identity.HasError())
        return Result<RenderMemoryCostPlan>::Failure(identity.ErrorValue());
    const auto &actual = identity.Value();
    const auto &expected = descriptor.compatibility;
    if (expected.backend != actual.backend || expected.backendModuleVersion != actual.backendModuleVersion ||
        expected.adapter != actual.adapter || expected.deviceCompatibilityDigest != actual.deviceCompatibilityDigest ||
        expected.driverIdentity != actual.driverIdentity || expected.driverVersion != actual.driverVersion)
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                      "Pipeline compatibility facts do not identify the selected OpenGL context."));
    const auto payload = vertexModule->second.cookedBytes + fragmentModule->second.cookedBytes;
    constexpr std::size_t expansion = 16, overhead = 128U * 1024U;
    if (payload > (std::numeric_limits<std::size_t>::max() - overhead) / expansion)
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
    const auto required = ConservativeRequirement(payload * expansion + overhead);
    if (!required)
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
    // Driver program allocations are opaque. This policy is estimated, not measured native memory.
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(4, payload, *required));
}

/** @copydoc IRenderResourceBackend::CreateGraphicsPipeline */
Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &descriptor, const std::uint64_t vertex,
                                             const std::uint64_t fragment, const RenderMemoryPlacement &placement) override {
    if (auto failure = ResourceCreationFailure(); failure)
        return std::move(*failure);
    if (frameActive_)
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::FrameActive));
    const auto cost = QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
    if (cost.HasError())
        return Result<std::uint64_t>::Failure(cost.ErrorValue());
    if (!MatchesPlacement(cost.Value(), placement))
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig));
    const auto layout = PrepareMaterialBindingLayout(descriptor.reflection);
    if (layout.HasError())
        return Result<std::uint64_t>::Failure(layout.ErrorValue());
    const auto key = ValidateRenderGraphicsPipelineDescriptor(descriptor);
    if (key.HasError())
        return Result<std::uint64_t>::Failure(key.ErrorValue());
    const auto features = functions_.materialExecution.queryFeatures();
    if (features.HasError())
        return Result<std::uint64_t>::Failure(features.ErrorValue());
    if (!features.Value().IsValid())
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidCapabilities));
    const auto program = Detail::CreateOpenGLGraphicsProgram(static_cast<std::uint32_t>(vertex), static_cast<std::uint32_t>(fragment),
                                                             descriptor, functions_.shaders, functions_.pipelines);
    if (program.HasError())
        return Result<std::uint64_t>::Failure(program.ErrorValue());
    try {
        prepared_.graphicsPipelines.emplace(program.Value(), Detail::OpenGLGraphicsPipeline{layout.Value(), descriptor.state, key.Value(),
                                                                                            features.Value()});
    } catch (const std::bad_alloc &) {
        functions_.shaders.deleteProgram(program.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL program tracking metadata allocation failed."));
    } catch (const std::length_error &) {
        functions_.shaders.deleteProgram(program.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL program tracking metadata exceeded its finite envelope."));
    }
    return Result<std::uint64_t>::Success(program.Value());
}

/** @copydoc IRenderResourceBackend::DestroyGraphicsPipeline */
void DestroyGraphicsPipeline(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread() || backendInstance > std::numeric_limits<std::uint32_t>::max())
        return;
    const auto program = static_cast<std::uint32_t>(backendInstance);
    if (prepared_.graphicsPipelines.erase(program) != 0)
        functions_.shaders.deleteProgram(program);
}

/** @copydoc IRenderResourceBackend::QueryShaderModuleMemoryCost */
Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &descriptor,
                                                         const std::size_t cookedBytes) const override {
    if (!IsOwnerThread())
        return WrongThread<RenderMemoryCostPlan>();
    const auto valid = ValidateRenderShaderModuleDescriptor(descriptor);
    if (valid.HasError())
        return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
    if (!initialized_ || !functions_.shaders.IsValid() || !functions_.shaders.isAvailable() ||
        descriptor.target.backend != ShaderTargetBackend::OpenGL || descriptor.target.payloadFormat != ShaderPayloadFormat::Glsl410 ||
        (descriptor.stage != ShaderStage::Vertex && descriptor.stage != ShaderStage::Fragment) ||
        descriptor.target.descriptorVersion != 1 || descriptor.target.interfaceSchemaVersion != 1 || cookedBytes == 0 ||
        cookedBytes > 64U * 1024U * 1024U)
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL admits cooked GLSL 410 vertex/fragment modules with version-one target/interface metadata."));
    // Driver shader allocations are opaque. This explicit admission estimate is not measured native memory usage.
    constexpr std::size_t expansion = 16;
    constexpr std::size_t objectEstimate = 64U * 1024U;
    if (cookedBytes > (std::numeric_limits<std::size_t>::max() - objectEstimate) / expansion)
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
    const auto required = ConservativeRequirement(cookedBytes * expansion + objectEstimate);
    if (!required)
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(3, cookedBytes, *required));
}

/** @copydoc IRenderResourceBackend::CreateShaderModule */
Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &descriptor, const std::span<const std::uint8_t> cookedCode,
                                         const RenderMemoryPlacement &placement) override {
    if (auto failure = ResourceCreationFailure(); failure)
        return std::move(*failure);
    if (frameActive_)
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::FrameActive));
    const auto cost = QueryShaderModuleMemoryCost(descriptor, cookedCode.size());
    if (cost.HasError())
        return Result<std::uint64_t>::Failure(cost.ErrorValue());
    if (!MatchesPlacement(cost.Value(), placement))
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::InvalidConfig, "Cooked shader candidate does not match its admitted memory placement."));
    const auto compiled = Detail::CompileOpenGLCookedStage({descriptor.stage, descriptor.entryPoint, cookedCode}, functions_.shaders);
    if (compiled.HasError())
        return Result<std::uint64_t>::Failure(compiled.ErrorValue());
    try {
        prepared_.shaderModules.emplace(compiled.Value(), Detail::OpenGLShaderModule{descriptor, cookedCode.size()});
    } catch (const std::bad_alloc &) {
        functions_.shaders.deleteShader(compiled.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL shader tracking metadata allocation failed."));
    } catch (const std::length_error &) {
        functions_.shaders.deleteShader(compiled.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL shader tracking metadata exceeded its finite envelope."));
    }
    return Result<std::uint64_t>::Success(compiled.Value());
}

/** @copydoc IRenderResourceBackend::DestroyShaderModule */
void DestroyShaderModule(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread() || backendInstance > std::numeric_limits<std::uint32_t>::max())
        return;
    const auto shader = static_cast<std::uint32_t>(backendInstance);
    if (prepared_.shaderModules.erase(shader) != 0)
        functions_.shaders.deleteShader(shader);
}

/** @copydoc IRenderResourceBackend::QuerySamplerMemoryCost */
Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const override {
    if (!IsOwnerThread())
        return WrongThread<RenderMemoryCostPlan>();
    if (const auto valid = ValidateRenderSamplerDescriptor(descriptor); valid.HasError())
        return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
    if (!initialized_ || !functions_.samplers.IsValid() || !functions_.samplers.isAvailable() || descriptor.maximumAnisotropy != 1.0F)
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    // Native sampler memory is opaque; this is an explicit estimated policy, not a driver measurement.
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(5, sizeof(RenderSamplerDescriptor), 4U * 1024U));
}

/** @copydoc IRenderResourceBackend::CreateSampler */
Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &descriptor, const RenderMemoryPlacement &placement) override {
    if (auto failure = ResourceCreationFailure(); failure)
        return std::move(*failure);
    if (frameActive_)
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::FrameActive));
    const auto cost = QuerySamplerMemoryCost(descriptor);
    if (cost.HasError())
        return Result<std::uint64_t>::Failure(cost.ErrorValue());
    if (!MatchesPlacement(cost.Value(), placement))
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig));
    const auto created = Detail::CreateOpenGLSampler(descriptor, functions_.samplers);
    if (created.HasError())
        return Result<std::uint64_t>::Failure(created.ErrorValue());
    try {
        prepared_.samplerDescriptors.emplace(created.Value(), descriptor);
    } catch (const std::bad_alloc &) {
        functions_.samplers.destroy(created.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL sampler tracking metadata allocation failed."));
    } catch (const std::length_error &) {
        functions_.samplers.destroy(created.Value());
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL sampler tracking metadata exceeded its finite envelope."));
    }
    return Result<std::uint64_t>::Success(created.Value());
}

/** @copydoc IRenderResourceBackend::DestroySampler */
void DestroySampler(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread() || backendInstance > std::numeric_limits<std::uint32_t>::max())
        return;
    const auto sampler = static_cast<std::uint32_t>(backendInstance);
    if (prepared_.samplerDescriptors.erase(sampler) != 0)
        functions_.samplers.destroy(sampler);
}

/** @copydoc IRenderBackend::QueryBufferMemoryCost */
Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
    const auto required = ConservativeRequirement(descriptor.byteSize);
    if (!initialized_ || !functions_.HasResourceFunctions() || !descriptor.IsValid() || !required.has_value())
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                               "OpenGL buffer memory requirements are unavailable for this descriptor."));
    if (!capabilities_.support.Supports(descriptor))
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL buffer memory-cost query failed: " +
                          DescribeRenderBufferAdmissionFailure(descriptor, capabilities_.support)));
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(1, descriptor.byteSize, *required));
}

/** @copydoc IRenderBackend::QueryTextureMemoryCost */
Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
    if (!initialized_ || !functions_.HasResourceFunctions())
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation, "OpenGL texture memory requirements are unavailable."));
    if (!capabilities_.support.formats.Supports(descriptor))
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL texture memory-cost query failed: " +
                          DescribeRenderTextureAdmissionFailure(descriptor, capabilities_.support)));
    if (!IsSupportedTextureDescriptor(descriptor))
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL texture memory-cost query failed: " + DescribeRenderTextureRequest(descriptor) +
                          "; this path implements only single-mip, single-layer, single-sample 2d textures with sampled or attachment "
                          "usage."));
    const auto payload = RenderTextureBaseLevelByteSize(descriptor);
    auto required = payload.has_value() ? ConservativeRequirement(*payload) : std::nullopt;
    const bool graphColor = capabilities_.supportsExactTransientResourceReuse && descriptor.format == RenderTextureFormat::Rgba8Unorm &&
                            HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment);
    if (graphColor && required.has_value()) {
        constexpr std::size_t attachmentEstimate = 256;
        required = *required <= std::numeric_limits<std::size_t>::max() - attachmentEstimate ? std::optional{*required + attachmentEstimate}
                                                                                             : std::nullopt;
    }
    if (!payload.has_value() || !required.has_value())
        return Result<RenderMemoryCostPlan>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                      "OpenGL texture memory-cost query failed: " + DescribeRenderTextureRequest(descriptor) +
                          "; memory requirement overflows the representable size."));
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(2, *payload, *required));
}

/** @copydoc IRenderBackend::CreateBuffer */
Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                   const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime = {}) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    const auto cost = QueryBufferMemoryCost(descriptor);
    if (cost.HasError())
        return Result<std::uint64_t>::Failure(cost.ErrorValue());
    if (!descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) ||
        !MatchesPlacement(cost.Value(), placement) ||
        descriptor.byteSize > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL buffer creation request is invalid."));
    if (functions_.state.error() != GL_NO_ERROR)
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
    std::array<std::int32_t, 1> previousBinding{};
    functions_.state.getInteger(GL_ARRAY_BUFFER_BINDING, previousBinding);
    const auto restoreBinding = [this](const std::int32_t *binding) noexcept {
        functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(*binding));
    };
    std::unique_ptr<const std::int32_t, decltype(restoreBinding)> bindingGuard{previousBinding.data(), restoreBinding};
    std::uint32_t buffer = 0;
    functions_.buffers.generateBuffers(1, &buffer);
    if (buffer == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a buffer object."));
    const auto deleteCandidate = [this](const std::uint32_t *object) noexcept {
        functions_.buffers.deleteBuffers(1, object);
        RetireNativeLifetime(NativeResourceKind::Buffer, *object);
    };
    std::unique_ptr<const std::uint32_t, decltype(deleteCandidate)> candidate{&buffer, deleteCandidate};
    if (auto tracked = TrackNativeLifetime(NativeResourceKind::Buffer, buffer, std::move(lifetime)); tracked.HasError())
        return Result<std::uint64_t>::Failure(std::move(tracked).ErrorValue());
    constexpr std::uint32_t target = GL_ARRAY_BUFFER;
    functions_.buffers.bindBuffer(target, buffer);
    functions_.buffers.bufferData(target, descriptor.byteSize, initialData, GL_STATIC_DRAW);
    if (functions_.state.error() != GL_NO_ERROR) {
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
    }
    try {
        buffers_.insert(buffer);
        bufferDescriptors_.insert_or_assign(buffer, descriptor);
    } catch (...) {  // NOSONAR(cpp:S2738)
        buffers_.erase(buffer);
        bufferDescriptors_.erase(buffer);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL buffer tracking allocation failed."));
    }
    static_cast<void>(candidate.release());
    return Result<std::uint64_t>::Success(buffer);
}

/** @copydoc IRenderBackend::CreateMesh */
Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer, const std::uint64_t indexBuffer,
                                 RenderResourceNativeLifetime lifetime = {}) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    // The initial OpenGL 4.1 path deliberately realizes only Horo's canonical MeshVertex layout;
    // arbitrary vertex declarations remain unsupported until a typed layout contract is introduced.
    if (!descriptor.IsValid() || descriptor.vertexStride != sizeof(MeshVertex) || vertexBuffer == 0 || indexBuffer == 0 ||
        vertexBuffer > std::numeric_limits<std::uint32_t>::max() || indexBuffer > std::numeric_limits<std::uint32_t>::max())
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL mesh creation request is invalid."));
    const auto vertex = bufferDescriptors_.find(static_cast<std::uint32_t>(vertexBuffer));
    const auto index = bufferDescriptors_.find(static_cast<std::uint32_t>(indexBuffer));
    if (vertex == bufferDescriptors_.end() || index == bufferDescriptors_.end())
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "OpenGL mesh references unknown buffer instances."));
    const std::size_t indexElementBytes = descriptor.indexFormat == RenderIndexFormat::UInt16 ? 2 : 4;
    const bool vertexRangeFits = descriptor.vertexCount <= vertex->second.byteSize / static_cast<std::size_t>(descriptor.vertexStride);
    const bool indexRangeFits = descriptor.indexCount <= index->second.byteSize / indexElementBytes;
    if (!HasBufferUsage(vertex->second.usage, RenderBufferUsage::Vertex) ||
        !HasBufferUsage(index->second.usage, RenderBufferUsage::Index) || !vertexRangeFits || !indexRangeFits)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "OpenGL mesh references unknown or incompatible buffers."));
    std::uint32_t vertexArray = 0;
    functions_.vertexArrays.generateVertexArrays(1, &vertexArray);
    if (vertexArray == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a mesh vertex array."));
    functions_.vertexArrays.bindVertexArray(0, vertexArray);
    functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(vertexBuffer));
    functions_.buffers.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<std::uint32_t>(indexBuffer));
    functions_.vertexArrays.vertexAttributePointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                   offsetof(MeshVertex, position));
    functions_.vertexArrays.enableVertexAttribute(0);
    functions_.vertexArrays.vertexAttributePointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                   offsetof(MeshVertex, normal));
    functions_.vertexArrays.enableVertexAttribute(1);
    functions_.vertexArrays.vertexAttributePointer(2, 2, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                   offsetof(MeshVertex, uv));
    functions_.vertexArrays.enableVertexAttribute(2);
    functions_.vertexArrays.bindVertexArray(0, 0);
    functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, 0);
    try {
        meshes_.insert(vertexArray);
        prepared_.meshDescriptors.emplace(vertexArray, descriptor);
    } catch (...) {  // NOSONAR(cpp:S2738)
        meshes_.erase(vertexArray);
        prepared_.meshDescriptors.erase(vertexArray);
        functions_.vertexArrays.deleteVertexArrays(1, &vertexArray);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL mesh tracking allocation failed."));
    }
    if (auto tracked = TrackNativeLifetime(NativeResourceKind::Mesh, vertexArray, std::move(lifetime)); tracked.HasError()) {
        DestroyMesh(vertexArray);
        return Result<std::uint64_t>::Failure(std::move(tracked).ErrorValue());
    }
    return Result<std::uint64_t>::Success(vertexArray);
}

/** @copydoc IRenderBackend::CreateTexture */
Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &textureDescriptor, const std::span<const std::byte> textureData,
                                    const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime = {}) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    const auto bytes = RenderTextureBaseLevelByteSize(textureDescriptor);
    const auto cost = QueryTextureMemoryCost(textureDescriptor);
    if (cost.HasError())
        return Result<std::uint64_t>::Failure(cost.ErrorValue());
    constexpr auto maximumExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (textureDescriptor.extent.width > maximumExtent || textureDescriptor.extent.height > maximumExtent ||
        textureDescriptor.extent.width > contextFacts_.maxTexture2DSize || textureDescriptor.extent.height > contextFacts_.maxTexture2DSize)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::InvalidConfig,
                      "OpenGL texture creation failed: " +
                          DescribeRenderTextureAdmissionFailure(textureDescriptor, capabilities_.support)));
    if (!bytes.has_value() || (!textureData.empty() && textureData.size() != *bytes) || !MatchesPlacement(cost.Value(), placement))
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture creation request is invalid."));
    std::uint32_t texture = 0;
    functions_.textures.generateTextures(1, &texture);
    if (texture == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a texture object."));
    if (auto tracked = TrackNativeLifetime(NativeResourceKind::Texture, texture, std::move(lifetime)); tracked.HasError()) {
        functions_.textures.deleteTextures(1, &texture);
        return Result<std::uint64_t>::Failure(std::move(tracked).ErrorValue());
    }
    functions_.textures.bindTexture(GL_TEXTURE_2D, texture);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The admitted descriptor has exactly one mip; sampler mip filtering must not make it incomplete.
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    const auto format = TextureFormat(textureDescriptor.format);
    functions_.textures.textureImage({
        .target = GL_TEXTURE_2D,
        .internalFormat = format.internal,
        .width = static_cast<std::int32_t>(textureDescriptor.extent.width),
        .height = static_cast<std::int32_t>(textureDescriptor.extent.height),
        .format = format.external,
        .type = format.type,
        .initialData = textureData,
    });
    functions_.textures.bindTexture(GL_TEXTURE_2D, 0);
    std::uint32_t graphFramebuffer{};
    if (capabilities_.supportsExactTransientResourceReuse && textureDescriptor.format == RenderTextureFormat::Rgba8Unorm &&
        HasTextureUsage(textureDescriptor.usage, RenderTextureUsage::RenderAttachment)) {
        const auto attachment = Detail::CreateOpenGLGraphFramebuffer(functions_, texture);
        if (attachment.HasError()) {
            functions_.textures.deleteTextures(1, &texture);
            RetireNativeLifetime(NativeResourceKind::Texture, texture);
            return Result<std::uint64_t>::Failure(attachment.ErrorValue());
        }
        graphFramebuffer = attachment.Value();
    }
    try {
        textures_.insert(texture);
        textureDescriptors_.insert_or_assign(texture, Detail::OpenGLGraphTexture{.descriptor = textureDescriptor,
                                                                                 .graphFramebuffer = graphFramebuffer});
    } catch (...) {  // NOSONAR(cpp:S2738)
        textures_.erase(texture);
        textureDescriptors_.erase(texture);
        if (graphFramebuffer != 0)
            functions_.framebuffers.deleteFramebuffers(1, &graphFramebuffer);
        functions_.textures.deleteTextures(1, &texture);
        RetireNativeLifetime(NativeResourceKind::Texture, texture);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL texture tracking allocation failed."));
    }
    return Result<std::uint64_t>::Success(texture);
}

/** @copydoc IRenderBackend::CreateTextureView */
Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &viewDescriptor, const std::uint64_t sourceTexture,
                                        RenderResourceNativeLifetime lifetime = {}) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    if (!viewDescriptor.IsValid() || sourceTexture == 0 || sourceTexture > std::numeric_limits<std::uint32_t>::max())
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture-view creation request is invalid."));
    const auto source = textureDescriptors_.find(static_cast<std::uint32_t>(sourceTexture));
    const bool baselineView = viewDescriptor.baseMip == 0 && viewDescriptor.mipCount == 1 && viewDescriptor.baseLayer == 0 &&
                              viewDescriptor.layerCount == 1 && viewDescriptor.dimension == RenderTextureViewDimension::TwoD;
    if (source == textureDescriptors_.end() || source->second.destroyRequested ||
        source->second.descriptor.format != viewDescriptor.format || !AspectMatches(viewDescriptor.format, viewDescriptor.aspect) ||
        !baselineView)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "OpenGL texture-view source, aspect, or range is incompatible."));
    const auto matchingView = std::ranges::find_if(textureViews_, [&](const auto &entry) {
        return entry.second.texture == sourceTexture && entry.second.format == viewDescriptor.format &&
               entry.second.aspect == viewDescriptor.aspect;
    });
    if (matchingView != textureViews_.end() && !lifetime.HasLease()) {
        auto &existing = matchingView->second;
        if (existing.references == std::numeric_limits<std::uint32_t>::max())
            return Result<std::uint64_t>::Failure(
                MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL texture-view reference capacity is exhausted."));
        ++existing.references;
        return Result<std::uint64_t>::Success(matchingView->first);
    }
    if (nextTextureViewIdentity_ == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL texture-view identity capacity is exhausted."));
    const std::uint64_t identity = (static_cast<std::uint64_t>(nextTextureViewIdentity_++) << 32U) | sourceTexture;
    try {
        textureViews_.emplace(identity, OpenGLTextureView{.texture = static_cast<std::uint32_t>(sourceTexture),
                                                          .extent = source->second.descriptor.extent,
                                                          .format = source->second.descriptor.format,
                                                          .aspect = viewDescriptor.aspect,
                                                          .usage = source->second.descriptor.usage,
                                                          .sampleCount = source->second.descriptor.sampleCount,
                                                          .references = 1});
    } catch (...) {  // NOSONAR(cpp:S2738)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL texture-view tracking allocation failed."));
    }
    if (auto tracked = TrackNativeLifetime(NativeResourceKind::View, identity, std::move(lifetime)); tracked.HasError()) {
        textureViews_.erase(identity);
        return Result<std::uint64_t>::Failure(std::move(tracked).ErrorValue());
    }
    return Result<std::uint64_t>::Success(identity);
}

/** @copydoc IRenderBackend::CreateRenderTarget */
Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                         const std::uint64_t depthAttachment, RenderResourceNativeLifetime lifetime = {}) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    if (!IsValidRenderTargetRequest(descriptor, colorAttachment, depthAttachment))
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL render-target creation request is invalid."));
    const auto color = colorAttachment == 0 ? textureViews_.end() : textureViews_.find(colorAttachment);
    const auto depth = depthAttachment == 0 ? textureViews_.end() : textureViews_.find(depthAttachment);
    if ((colorAttachment != 0 && (color == textureViews_.end() || !IsCompatibleAttachment(color->second, descriptor, true))) ||
        (depthAttachment != 0 && (depth == textureViews_.end() || !IsCompatibleAttachment(depth->second, descriptor, false))))
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "OpenGL render-target attachment is unknown or incompatible."));
    std::uint32_t framebuffer = 0;
    functions_.framebuffers.generateFramebuffers(1, &framebuffer);
    if (framebuffer == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a framebuffer object."));
    functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    if (colorAttachment != 0)
        functions_.framebuffers.framebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color->second.texture, 0);
    else {
        functions_.framebuffers.drawBuffer(GL_NONE);
        functions_.framebuffers.readBuffer(GL_NONE);
    }
    if (const Result<void> attached = AttachDepthView(depthAttachment); attached.HasError()) {
        functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, 0);
        functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
        return Result<std::uint64_t>::Failure(attached.ErrorValue());
    }
    const bool complete = functions_.framebuffers.checkFramebuffer(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
        functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL framebuffer attachments are incomplete."));
    }
    try {
        renderTargets_.insert(framebuffer);
        const auto depth = textureViews_.find(depthAttachment);
        prepared_.materialTargets
            .emplace(framebuffer,
                     Detail::OpenGLMaterialTarget{.extent = descriptor.extent,
                                                  .colorTexture = colorAttachment != 0 ? color->second.texture : 0,
                                                  .depthTexture = depth != textureViews_.end() ? depth->second.texture : 0,
                                                  .colorFormat =
                                                      colorAttachment != 0 ? color->second.format : RenderTextureFormat::Rgba8Unorm,
                                                  .depthFormat =
                                                      depth != textureViews_.end() ? std::optional{depth->second.format} : std::nullopt,
                                                  .sampleCount = descriptor.sampleCount});
    } catch (...) {  // NOSONAR(cpp:S2738)
        renderTargets_.erase(framebuffer);
        prepared_.materialTargets.erase(framebuffer);
        functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL render-target tracking allocation failed."));
    }
    if (auto tracked = TrackNativeLifetime(NativeResourceKind::Target, framebuffer, std::move(lifetime)); tracked.HasError()) {
        DestroyRenderTarget(framebuffer);
        return Result<std::uint64_t>::Failure(std::move(tracked).ErrorValue());
    }
    return Result<std::uint64_t>::Success(framebuffer);
}

/** @brief Attaches one validated backend-private depth view to the bound framebuffer. */
[[nodiscard]] Result<void> AttachDepthView(const std::uint64_t depthAttachment) const {
    if (depthAttachment == 0)
        return Result<void>::Success();
    const auto view = textureViews_.find(depthAttachment);
    if (view == textureViews_.end())
        return Result<void>::Failure(
            MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "OpenGL render-target depth view metadata is unavailable."));
    const std::uint32_t attachment =
        view->second.format == RenderTextureFormat::Depth24Stencil8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    functions_.framebuffers.framebufferTexture(GL_FRAMEBUFFER, attachment, view->second.texture, 0);
    return Result<void>::Success();
}

/** @brief Validates the backend-private attachment presence required for target realization. */
[[nodiscard]] static bool IsValidRenderTargetRequest(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) noexcept {
    return descriptor.IsValid() && (colorAttachment != 0 || depthAttachment != 0);
}

/** @copydoc IRenderBackend::DestroyBuffer */
void DestroyBuffer(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    if (backendInstance <= std::numeric_limits<std::uint32_t>::max())
        bufferDescriptors_.erase(static_cast<std::uint32_t>(backendInstance));
    DeleteTrackedObject(functions_.buffers.deleteBuffers, buffers_, backendInstance);
    RetireNativeLifetime(NativeResourceKind::Buffer, backendInstance);
}

/** @copydoc IRenderBackend::DestroyMesh */
void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    if (backendInstance <= std::numeric_limits<std::uint32_t>::max())
        prepared_.meshDescriptors.erase(static_cast<std::uint32_t>(backendInstance));
    DeleteTrackedObject(functions_.vertexArrays.deleteVertexArrays, meshes_, backendInstance);
    RetireNativeLifetime(NativeResourceKind::Mesh, backendInstance);
}

/** @copydoc IRenderBackend::DestroyTexture */
void DestroyTexture(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    if (backendInstance == 0 || backendInstance > std::numeric_limits<std::uint32_t>::max())
        return;
    const auto texture = static_cast<std::uint32_t>(backendInstance);
    const auto tracked = textureDescriptors_.find(texture);
    if (tracked == textureDescriptors_.end())
        return;
    tracked->second.destroyRequested = true;
    TryDestroyPendingTexture(texture);
}

/** @copydoc IRenderBackend::DestroyTextureView */
void DestroyTextureView(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    const auto view = textureViews_.find(backendInstance);
    if (view == textureViews_.end())
        return;
    const std::uint32_t texture = view->second.texture;
    if (view->second.references > 1)
        --view->second.references;
    else {
        textureViews_.erase(view);
        RetireNativeLifetime(NativeResourceKind::View, backendInstance);
    }
    TryDestroyPendingTexture(texture);
}

/** @copydoc IRenderBackend::DestroyRenderTarget */
void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    if (backendInstance <= std::numeric_limits<std::uint32_t>::max())
        prepared_.materialTargets.erase(static_cast<std::uint32_t>(backendInstance));
    DeleteTrackedObject(functions_.framebuffers.deleteFramebuffers, renderTargets_, backendInstance);
    RetireNativeLifetime(NativeResourceKind::Target, backendInstance);
}
