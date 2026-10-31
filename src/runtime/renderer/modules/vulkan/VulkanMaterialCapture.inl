/** @brief Restricts offscreen material attachments to whole single-sample images. */
template <typename View> bool WholeMaterialView(const View &view) {
    if (!view)
        return true;
    const auto &texture = view->texture->descriptor;
    const auto &descriptor = view->descriptor;
    const bool stencil =
        texture.format == RenderTextureFormat::Depth24Stencil8 || texture.format == RenderTextureFormat::Depth32FloatStencil8;
    const bool validAspect = view->aspect == RenderTextureAspect::Color ||
                             view->aspect == (stencil ? RenderTextureAspect::DepthStencil : RenderTextureAspect::Depth);
    const std::array valid{texture.dimension == RenderTextureDimension::TwoD,
                           descriptor.dimension == RenderTextureViewDimension::TwoD,
                           texture.sampleCount == 1,
                           texture.layerCount == 1,
                           texture.mipCount == 1,
                           descriptor.baseMip == 0,
                           descriptor.baseLayer == 0,
                           descriptor.mipCount == 1,
                           descriptor.layerCount == 1,
                           validAspect};
    return std::ranges::all_of(valid, [](bool value) {
        return value;
    });
}

/** @copydoc VulkanMaterialCapture::CaptureMaterialTarget */
Result<VulkanMaterialTarget> VulkanMaterialCapture::CaptureMaterialTarget(const std::uint64_t identity,
                                                                          VulkanCommandRetention &retention) const {
    const auto found = runtime_.impl_->targets.find(identity);
    if (found == runtime_.impl_->targets.end() || !found->second->color || retention.resources.size() == retention.resources.capacity())
        return Result<VulkanMaterialTarget>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto &target = *found->second;
    if (!WholeMaterialView(target.color) || !WholeMaterialView(target.depth))
        return Result<VulkanMaterialTarget>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported));
    VulkanMaterialTarget result;
    const auto capture = [](const auto &view) {
        return VulkanGraphResource{.kind = RenderGraphResourceKind::Texture,
                                   .image = view->texture->handle,
                                   .textureDescriptor = view->texture->descriptor,
                                   .state = &view->texture->graphState};
    };
    result.color = capture(target.color);
    result.colorView = target.color->handle;
    if (target.depth) {
        result.depth = capture(target.depth);
        result.depthView = target.depth->handle;
    }
    result.extent = target.extent;
    retention.resources.push_back(found->second);
    return Result<VulkanMaterialTarget>::Success(result);
}

/** @copydoc VulkanMaterialCapture::CaptureMaterialMesh */
Result<VulkanMaterialMesh> VulkanMaterialCapture::CaptureMaterialMesh(const std::uint64_t identity,
                                                                      VulkanCommandRetention &retention) const {
    const auto found = runtime_.impl_->meshes.find(identity);
    if (found == runtime_.impl_->meshes.end() || retention.resources.capacity() - retention.resources.size() < 3)
        return Result<VulkanMaterialMesh>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto vertex = runtime_.impl_->buffers.find(found->second->vertex);
    const auto index = runtime_.impl_->buffers.find(found->second->index);
    if (vertex == runtime_.impl_->buffers.end() || index == runtime_.impl_->buffers.end())
        return Result<VulkanMaterialMesh>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto capture = [](const auto &buffer) {
        return VulkanGraphResource{.buffer = buffer->handle, .bufferDescriptor = buffer->descriptor, .state = &buffer->graphState};
    };
    VulkanMaterialMesh result{capture(vertex->second), capture(index->second), found->second->descriptor};
    retention.resources.push_back(found->second);
    retention.resources.push_back(vertex->second);
    retention.resources.push_back(index->second);
    return Result<VulkanMaterialMesh>::Success(result);
}

/** @copydoc VulkanMaterialCapture::ResolveDrawView */
Result<VulkanDrawViewBinding> VulkanMaterialCapture::ResolveDrawView(const std::uint64_t identity) const {
    const auto *view = VulkanResourceRuntime::Impl::Find(runtime_.impl_->views, identity);
    if (!view || !view->texture)
        return Result<VulkanDrawViewBinding>::Failure(
            ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "Draw attachment view is foreign or retired."));
    return Result<VulkanDrawViewBinding>::Success({view->handle, view->texture->handle, view->descriptor});
}

/** @copydoc VulkanResourceRuntime::CaptureGuiBuffer */
Result<VulkanGuiBuffer> VulkanResourceRuntime::CaptureGuiBuffer(const std::uint64_t instance, VulkanCommandRetention &retention) const {
    const auto found = impl_->buffers.find(instance);
    if (found == impl_->buffers.end() || retention.resources.size() == retention.resources.capacity())
        return Result<VulkanGuiBuffer>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto &buffer = *found->second;
    if (buffer.descriptor.access != RenderBufferAccess::HostVisible)
        return Result<VulkanGuiBuffer>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported));
    retention.resources.push_back(found->second);
    return Result<VulkanGuiBuffer>::Success(
        {{.buffer = buffer.handle, .bufferDescriptor = buffer.descriptor, .state = &found->second->graphState}, buffer.memory});
}

/** @copydoc VulkanResourceRuntime::CaptureGuiSample */
Result<VulkanGuiSample> VulkanResourceRuntime::CaptureGuiSample(const GuiSampledResourceInstance &source,
                                                                VulkanCommandRetention &retention) const {
    const auto found = impl_->views.find(source.view);
    if (found == impl_->views.end() || retention.resources.capacity() - retention.resources.size() < 3)
        return Result<VulkanGuiSample>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto &view = *found->second;
    const auto *external = view.texture->externalSource;
    const bool mirror = external != nullptr && external->AdmitsFirstEyeGuiMirror(view.descriptor);
    if (view.aspect != RenderTextureAspect::Color || (external != nullptr ? !mirror : !WholeMaterialView(found->second)) ||
        !HasTextureUsage(view.texture->descriptor.usage, RenderTextureUsage::Sampled) || (mirror && source.uploadSource != 0))
        return Result<VulkanGuiSample>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported));
    if (mirror) {
        if (auto current = external->Validate(); current.HasError())
            return Result<VulkanGuiSample>::Failure(current.ErrorValue());
    }
    const auto sampler = shaders_.CaptureGuiSampler(source.sampler);
    if (sampler.HasError())
        return Result<VulkanGuiSample>::Failure(sampler.ErrorValue());
    VulkanGuiSample result{.texture = {.kind = RenderGraphResourceKind::Texture,
                                       .image = view.texture->handle,
                                       .textureDescriptor = view.texture->descriptor,
                                       .state = &view.texture->graphState},
                           .view = view.handle,
                           .sampler = sampler.Value().handle};
    if (mirror) {
        result.requiredFinal = external->RequiredFinalState();
        result.requiredFinal->scope = RenderGraphPipelineScope::Graphics;
    }
    if (source.uploadSource != 0) {
        const auto staging = CaptureGuiBuffer(source.uploadSource, retention);
        if (staging.HasError())
            return Result<VulkanGuiSample>::Failure(staging.ErrorValue());
        const auto &resource = staging.Value().resource;
        const auto bytes = RenderTextureBaseLevelByteSize(view.texture->descriptor);
        if (!bytes || resource.bufferDescriptor.byteSize != *bytes ||
            !HasBufferUsage(resource.bufferDescriptor.usage, RenderBufferUsage::CopySource) ||
            !HasTextureUsage(view.texture->descriptor.usage, RenderTextureUsage::CopyDestination))
            return Result<VulkanGuiSample>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
        result.staging = resource;
    }
    retention.resources.push_back(found->second);
    retention.resources.push_back(sampler.Value().backing);
    return Result<VulkanGuiSample>::Success(result);
}

/** @copydoc VulkanResourceRuntime::BufferCompletion */
Result<RenderTimelinePoint> VulkanResourceRuntime::BufferCompletion(const std::uint64_t instance) const {
    const auto *buffer = Impl::Find(impl_->buffers, instance);
    if (!buffer)
        return Result<RenderTimelinePoint>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    return Result<RenderTimelinePoint>::Success(buffer->graphState.completion);
}

/** @copydoc VulkanResourceRuntime::WriteHostFrameBuffer */
Result<void> VulkanResourceRuntime::WriteHostFrameBuffer(const std::uint64_t instance, const std::span<const std::byte> bytes) {
    auto *buffer = Impl::Find(impl_->buffers, instance);
    if (!buffer || buffer->descriptor.access != RenderBufferAccess::HostVisible ||
        (!HasBufferUsage(buffer->descriptor.usage, RenderBufferUsage::Uniform) && buffer->descriptor.usage != RenderBufferUsage::Vertex) ||
        bytes.size() != buffer->descriptor.byteSize)
        return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
    buffer->vfxPublished = false;
    if (!impl_->Upload(*buffer, bytes))
        return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
    buffer->graphState.contentsDefined = true;
    return Result<void>::Success();
}

/** @copydoc VulkanResourceRuntime::VfxPublication */
Result<VulkanVfxPublication> VulkanResourceRuntime::VfxPublication(const std::uint64_t instance) const {
    const auto *buffer = Impl::Find(impl_->buffers, instance);
    if (!buffer)
        return Result<VulkanVfxPublication>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    return Result<VulkanVfxPublication>::Success({buffer->handle, buffer->vfxRevision, buffer->vfxInstances, buffer->vfxPublished});
}

/** @copydoc VulkanResourceRuntime::UpdateVfxFrame */
Result<void> VulkanResourceRuntime::UpdateVfxFrame(const NativeVfxFrameUpdate &update, const bool &closing) {
    if (closing || !update.layout || !update.writeStatus || *update.writeStatus != VfxFrameWriteStatus::NotStarted ||
        update.revision == 0 || update.instanceCount == 0 || update.instanceCount > MaximumVfxDrawInstances)
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    const auto *material = dynamic_cast<const VulkanPreparedMaterialBinding *>(update.material);
    const auto found = impl_->buffers.find(update.instance);
    if (!material || found == impl_->buffers.end() || update.deviceIncarnation != material->deviceIncarnation)
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    if (const auto valid = shaders_.ValidateVfxMaterial(*material, update.pipeline, *update.layout); valid.HasError())
        return valid;
    // Pin the existing generation across native map/unmap; this does not create another resource owner or identity.
    const auto buffer = found->second;
    if (buffer->descriptor.usage != RenderBufferUsage::Uniform || buffer->descriptor.access != RenderBufferAccess::HostVisible ||
        update.parameters.size() != buffer->descriptor.byteSize || update.revision <= buffer->vfxRevision)
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    std::size_t matches{};
    for (const auto &resource : material->resources)
        if (resource.buffer == buffer->handle) {
            if (resource.binding != VfxShaderParameterIds::Instances || resource.offset != 0 ||
                resource.byteCount != update.parameters.size() || resource.backing.get() != buffer.get())
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            ++matches;
        }
    if (matches != 1)
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    if (update.cancellation.IsCancellationRequested())
        return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
    // Native dispatch can be injected in qualification. Revalidate the same registry generation after map,
    // before writing, while the canonical command owner's borrow prevents device retirement on shutdown.
    void *mapped{};
    const auto device = impl_->device;
    if (impl_->dispatch.mapMemory(device, buffer->memory, 0, update.parameters.size(), 0, &mapped) != VK_SUCCESS)
        return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
    const auto current = impl_->buffers.find(update.instance);
    if (closing || current == impl_->buffers.end() || current->second.get() != buffer.get() || !mapped ||
        update.cancellation.IsCancellationRequested() ||
        shaders_.ValidateVfxMaterial(*material, update.pipeline, *update.layout).HasError()) {
        impl_->dispatch.unmapMemory(device, buffer->memory);
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    }
    *update.writeStatus = VfxFrameWriteStatus::Attempted;
    buffer->vfxPublished = false;
    std::ranges::copy(update.parameters, static_cast<std::byte *>(mapped));
    impl_->dispatch.unmapMemory(device, buffer->memory);
    const auto accepted = impl_->buffers.find(update.instance);
    if (closing || accepted == impl_->buffers.end() || accepted->second.get() != buffer.get() ||
        shaders_.ValidateVfxMaterial(*material, update.pipeline, *update.layout).HasError())
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    buffer->graphState.contentsDefined = true;
    buffer->vfxRevision = update.revision;
    buffer->vfxInstances = update.instanceCount;
    buffer->vfxPublished = true;
    *update.writeStatus = VfxFrameWriteStatus::Committed;
    return Result<void>::Success();
}
