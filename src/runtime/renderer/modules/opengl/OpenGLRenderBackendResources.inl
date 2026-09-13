/** @brief Resource realization methods kept with OpenGLRenderBackend's private definition. */
/** @copydoc IRenderBackend::QueryBufferMemoryCost */
Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
    const auto required = ConservativeRequirement(descriptor.byteSize);
    if (!initialized_ || !functions_.HasResourceFunctions() || !descriptor.IsValid() || !required.has_value())
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                               "OpenGL buffer memory requirements are unavailable for this descriptor."));
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(1, descriptor.byteSize, *required));
}

/** @copydoc IRenderBackend::QueryTextureMemoryCost */
Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
    const auto payload = RenderTextureBaseLevelByteSize(descriptor);
    const auto required = payload.has_value() ? ConservativeRequirement(*payload) : std::nullopt;
    if (!initialized_ || !functions_.HasResourceFunctions() || !payload.has_value() || !required.has_value() ||
        !IsSupportedTextureDescriptor(descriptor))
        return Result<RenderMemoryCostPlan>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                               "OpenGL texture memory requirements are unavailable for this descriptor."));
    return Result<RenderMemoryCostPlan>::Success(MemoryCostPlan(2, *payload, *required));
}

/** @copydoc IRenderBackend::CreateBuffer */
Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                   const RenderMemoryPlacement &placement) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    if (const auto cost = QueryBufferMemoryCost(descriptor);
        !descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) || cost.HasError() ||
        !MatchesPlacement(cost.Value(), placement) ||
        descriptor.byteSize > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL buffer creation request is invalid."));
    std::uint32_t buffer = 0;
    functions_.buffers.generateBuffers(1, &buffer);
    if (buffer == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a buffer object."));
    constexpr std::uint32_t target = GL_ARRAY_BUFFER;
    functions_.buffers.bindBuffer(target, buffer);
    functions_.buffers.bufferData(target, descriptor.byteSize, initialData, GL_STATIC_DRAW);
    functions_.buffers.bindBuffer(target, 0);
    try {
        buffers_.insert(buffer);
        bufferDescriptors_.insert_or_assign(buffer, descriptor);
    } catch (...) {  // NOSONAR(cpp:S2738)
        buffers_.erase(buffer);
        bufferDescriptors_.erase(buffer);
        functions_.buffers.deleteBuffers(1, &buffer);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL buffer tracking allocation failed."));
    }
    return Result<std::uint64_t>::Success(buffer);
}

/** @copydoc IRenderBackend::CreateMesh */
Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                 const std::uint64_t indexBuffer) override {
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
    } catch (...) {  // NOSONAR(cpp:S2738)
        functions_.vertexArrays.deleteVertexArrays(1, &vertexArray);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL mesh tracking allocation failed."));
    }
    return Result<std::uint64_t>::Success(vertexArray);
}

/** @copydoc IRenderBackend::CreateTexture */
Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &textureDescriptor, const std::span<const std::byte> textureData,
                                    const RenderMemoryPlacement &placement) override {
    if (auto failure = ResourceCreationFailure(); failure.has_value())
        return std::move(*failure);
    const auto bytes = RenderTextureBaseLevelByteSize(textureDescriptor);
    const auto cost = QueryTextureMemoryCost(textureDescriptor);
    if (constexpr auto maximumExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        !bytes.has_value() || (!textureData.empty() && textureData.size() != *bytes) || cost.HasError() ||
        !MatchesPlacement(cost.Value(), placement) || textureDescriptor.extent.width > maximumExtent ||
        textureDescriptor.extent.height > maximumExtent || textureDescriptor.extent.width > contextFacts_.maxTexture2DSize ||
        textureDescriptor.extent.height > contextFacts_.maxTexture2DSize)
        return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture creation request is invalid."));
    std::uint32_t texture = 0;
    functions_.textures.generateTextures(1, &texture);
    if (texture == 0)
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL failed to allocate a texture object."));
    functions_.textures.bindTexture(GL_TEXTURE_2D, texture);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
    try {
        textures_.insert(texture);
        textureDescriptors_.insert_or_assign(texture, OpenGLTrackedTexture{.descriptor = textureDescriptor});
    } catch (...) {  // NOSONAR(cpp:S2738)
        textures_.erase(texture);
        textureDescriptors_.erase(texture);
        functions_.textures.deleteTextures(1, &texture);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL texture tracking allocation failed."));
    }
    return Result<std::uint64_t>::Success(texture);
}

/** @copydoc IRenderBackend::CreateTextureView */
Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &viewDescriptor, const std::uint64_t sourceTexture) override {
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
    if (matchingView != textureViews_.end()) {
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
    return Result<std::uint64_t>::Success(identity);
}

/** @copydoc IRenderBackend::CreateRenderTarget */
Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                         const std::uint64_t depthAttachment) override {
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
    } catch (...) {  // NOSONAR(cpp:S2738)
        functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
        return Result<std::uint64_t>::Failure(
            MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL render-target tracking allocation failed."));
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
}

/** @copydoc IRenderBackend::DestroyMesh */
void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    DeleteTrackedObject(functions_.vertexArrays.deleteVertexArrays, meshes_, backendInstance);
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
    else
        textureViews_.erase(view);
    TryDestroyPendingTexture(texture);
}

/** @copydoc IRenderBackend::DestroyRenderTarget */
void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
    if (!IsOwnerThread())
        return;
    DeleteTrackedObject(functions_.framebuffers.deleteFramebuffers, renderTargets_, backendInstance);
}
