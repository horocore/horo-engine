/** @brief Resolves an exact native uniform generation without taking resource ownership from the frontend. */
Result<VulkanMaterialResource> VulkanResourceRuntime::Impl::ResolveMaterialBuffer(const MaterialResidentResource &resident) const {
    const auto *buffer = Find(buffers, resident.instance);
    if (resident.kind == MaterialResidentResourceKind::Buffer && buffer &&
        HasBufferUsage(buffer->descriptor.usage, RenderBufferUsage::Uniform) && resident.offset <= buffer->descriptor.byteSize &&
        resident.byteCount <= buffer->descriptor.byteSize - resident.offset)
        return Result<VulkanMaterialResource>::Success({.buffer = buffer->handle,
                                                        .bytes = buffer->descriptor.byteSize,
                                                        .bufferDescriptor = buffer->descriptor,
                                                        .state = &buffers.at(resident.instance)->graphState,
                                                        .backing = buffers.at(resident.instance)});
    return Result<VulkanMaterialResource>::Failure(
        MakeError(VulkanBackendErrors::ResourceIdentityInvalid,
                  "Material uniform generation, usage or range is incompatible with the selected Vulkan registry."));
}

/** @brief Resolves an exact sampled color view in the current single-sample 2D material envelope. */
Result<VulkanMaterialResource> VulkanResourceRuntime::Impl::ResolveMaterialView(const MaterialResidentResource &resident) const {
    const auto *view = Find(views, resident.instance);
    if (resident.kind != MaterialResidentResourceKind::TextureView || resident.offset || resident.byteCount || !view)
        return Result<VulkanMaterialResource>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto &descriptor = view->texture->descriptor;
    if (view->aspect == RenderTextureAspect::Color && HasTextureUsage(descriptor.usage, RenderTextureUsage::Sampled) &&
        descriptor.sampleCount == 1 && descriptor.dimension == RenderTextureDimension::TwoD && descriptor.layerCount == 1 &&
        view->descriptor.dimension == RenderTextureViewDimension::TwoD)
        return Result<VulkanMaterialResource>::Success({.view = view->handle,
                                                        .image = view->texture->handle,
                                                        .textureDescriptor = descriptor,
                                                        .state = &view->texture->graphState,
                                                        .backing = views.at(resident.instance)});
    return Result<VulkanMaterialResource>::Failure(
        MakeError(VulkanBackendErrors::ResourceIdentityInvalid,
                  "Material texture view is not an exact sampled single-sample 2D color generation."));
}

/** @copydoc VulkanResourceRuntime::InitializeShaders */
Result<void> VulkanResourceRuntime::InitializeShaders(const VkPhysicalDeviceProperties &properties, RenderAdapterId adapter,
                                                      const VulkanShaderRuntime::Resolver &resolver, const std::uint64_t deviceIncarnation,
                                                      const bool vertexLayerEnabled) {
    return shaders_.Initialize(impl_->device, properties, std::move(adapter), resolver, [this] {
        return impl_->IssueIdentity();
    }, [this](const MaterialResidentResource &resident, ShaderResourceKind kind) {
        return kind == ShaderResourceKind::UniformBuffer ? impl_->ResolveMaterialBuffer(resident) : impl_->ResolveMaterialView(resident);
    }, VulkanShaderRuntime::DeviceFeatures{deviceIncarnation, vertexLayerEnabled});
}

/** @copydoc VulkanResourceRuntime::QueryPipelineDeviceIdentity */
Result<RenderPipelineDeviceIdentity> VulkanResourceRuntime::QueryPipelineDeviceIdentity() const {
    return shaders_.QueryPipelineDeviceIdentity();
}

/** @copydoc VulkanResourceRuntime::QueryShaderModuleMemoryCost */
Result<RenderMemoryCostPlan> VulkanResourceRuntime::QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &d,
                                                                                std::size_t n) const {
    return shaders_.QueryShaderModuleMemoryCost(d, n);
}

/** @copydoc VulkanResourceRuntime::QueryGraphicsPipelineMemoryCost */
Result<RenderMemoryCostPlan> VulkanResourceRuntime::QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &d,
                                                                                    std::uint64_t v, std::uint64_t f) const {
    return shaders_.QueryGraphicsPipelineMemoryCost(d, v, f);
}

/** @copydoc VulkanResourceRuntime::QuerySamplerMemoryCost */
Result<RenderMemoryCostPlan> VulkanResourceRuntime::QuerySamplerMemoryCost(const RenderSamplerDescriptor &d) const {
    return shaders_.QuerySamplerMemoryCost(d);
}

/** @copydoc VulkanResourceRuntime::CreateShaderModule */
Result<std::uint64_t> VulkanResourceRuntime::CreateShaderModule(const RenderShaderModuleDescriptor &d, std::span<const std::uint8_t> c,
                                                                const RenderMemoryPlacement &p) {
    return shaders_.CreateShaderModule(d, c, p);
}

/** @copydoc VulkanResourceRuntime::CreateGraphicsPipeline */
Result<std::uint64_t> VulkanResourceRuntime::CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &d, std::uint64_t v,
                                                                    std::uint64_t f, const RenderMemoryPlacement &p) {
    return shaders_.CreateGraphicsPipeline(d, v, f, p);
}

/** @copydoc VulkanResourceRuntime::CreateSampler */
Result<std::uint64_t> VulkanResourceRuntime::CreateSampler(const RenderSamplerDescriptor &d, const RenderMemoryPlacement &p) {
    return shaders_.CreateSampler(d, p);
}

/** @copydoc VulkanResourceRuntime::DestroyShaderModule */
void VulkanResourceRuntime::DestroyShaderModule(std::uint64_t id) noexcept {
    shaders_.DestroyShaderModule(id);
}

/** @copydoc VulkanResourceRuntime::DestroyGraphicsPipeline */
void VulkanResourceRuntime::DestroyGraphicsPipeline(std::uint64_t id) noexcept {
    shaders_.DestroyGraphicsPipeline(id);
}

/** @copydoc VulkanResourceRuntime::DestroySampler */
void VulkanResourceRuntime::DestroySampler(std::uint64_t id) noexcept {
    shaders_.DestroySampler(id);
}

/** @copydoc VulkanResourceRuntime::QueryMaterialParameterBufferSize */
Result<std::size_t> VulkanResourceRuntime::QueryMaterialParameterBufferSize(std::uint64_t id, const MaterialBindingDescriptor &d,
                                                                            ShaderBindingId b) const {
    return shaders_.QueryMaterialParameterBufferSize(id, d, b);
}

/** @copydoc VulkanResourceRuntime::RealizeResolved */
Result<std::unique_ptr<IResidentMaterialBinding>> VulkanResourceRuntime::RealizeResolved(const MaterialBindingDescriptor &d,
                                                                                         std::uint64_t id,
                                                                                         std::span<const MaterialResidentResource> r) {
    return shaders_.RealizeResolved(d, id, r);
}

/** @copydoc VulkanResourceRuntime::ValidateVfxMaterial */
Result<void> VulkanResourceRuntime::ValidateVfxMaterial(const VulkanPreparedMaterialBinding &material,
                                                        const MaterialBindingLayout &layout) const {
    return shaders_.ValidateVfxMaterial(material, material.pipelineIdentity, layout);
}
