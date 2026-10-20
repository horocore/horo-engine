/** @copydoc IRenderResourceBackend::QueryPipelineDeviceIdentity */
Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const override {
    return initialized_ && resourceBackend_ ? resourceBackend_->QueryPipelineDeviceIdentity() : Unsupported<RenderPipelineDeviceIdentity>();
}

/** @copydoc IRenderResourceBackend::QueryShaderModuleMemoryCost */
Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &d, std::size_t n) const override {
    return initialized_ && resourceBackend_ ? resourceBackend_->QueryShaderModuleMemoryCost(d, n) : Unsupported<RenderMemoryCostPlan>();
}

/** @copydoc IRenderResourceBackend::QueryGraphicsPipelineMemoryCost */
Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &d, std::uint64_t v,
                                                             std::uint64_t f) const override {
    return initialized_ && resourceBackend_ ? resourceBackend_->QueryGraphicsPipelineMemoryCost(d, v, f)
                                            : Unsupported<RenderMemoryCostPlan>();
}

/** @copydoc IRenderResourceBackend::QuerySamplerMemoryCost */
Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &d) const override {
    return initialized_ && resourceBackend_ ? resourceBackend_->QuerySamplerMemoryCost(d) : Unsupported<RenderMemoryCostPlan>();
}

/** @copydoc IRenderResourceBackend::CreateShaderModule */
Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &d, std::span<const std::uint8_t> c,
                                         const RenderMemoryPlacement &p) override {
    return initialized_ && resourceBackend_ ? resourceBackend_->CreateShaderModule(d, c, p) : Unsupported<std::uint64_t>();
}

/** @copydoc IRenderResourceBackend::CreateGraphicsPipeline */
Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &d, std::uint64_t v, std::uint64_t f,
                                             const RenderMemoryPlacement &p) override {
    return initialized_ && resourceBackend_ ? resourceBackend_->CreateGraphicsPipeline(d, v, f, p) : Unsupported<std::uint64_t>();
}

/** @copydoc IRenderResourceBackend::CreateSampler */
Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &d, const RenderMemoryPlacement &p) override {
    return initialized_ && resourceBackend_ ? resourceBackend_->CreateSampler(d, p) : Unsupported<std::uint64_t>();
}

/** @copydoc IRenderResourceBackend::DestroyShaderModule */
void DestroyShaderModule(std::uint64_t id) noexcept override {
    if (resourceBackend_)
        resourceBackend_->DestroyShaderModule(id);
}

/** @copydoc IRenderResourceBackend::DestroyGraphicsPipeline */
void DestroyGraphicsPipeline(std::uint64_t id) noexcept override {
    if (resourceBackend_)
        resourceBackend_->DestroyGraphicsPipeline(id);
}

/** @copydoc IRenderResourceBackend::DestroySampler */
void DestroySampler(std::uint64_t id) noexcept override {
    if (resourceBackend_)
        resourceBackend_->DestroySampler(id);
}

/** @copydoc IMaterialBindingBackend::QueryMaterialParameterBufferSize */
Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t id, const MaterialBindingDescriptor &d,
                                                     ShaderBindingId b) const override {
    auto *material = dynamic_cast<IMaterialBindingBackend *>(resourceBackend_);
    return initialized_ && material ? material->QueryMaterialParameterBufferSize(id, d, b) : Unsupported<std::size_t>();
}

/** @copydoc IMaterialBindingBackend::RealizeResolved */
Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &d, std::uint64_t id,
                                                                  std::span<const MaterialResidentResource> r) override {
    auto *material = dynamic_cast<IMaterialBindingBackend *>(resourceBackend_);
    return initialized_ && material ? material->RealizeResolved(d, id, r) : Unsupported<std::unique_ptr<IResidentMaterialBinding>>();
}
