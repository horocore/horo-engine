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

/** @copydoc VulkanResourceRuntime::CaptureMaterialTarget */
Result<VulkanMaterialTarget> VulkanResourceRuntime::CaptureMaterialTarget(const std::uint64_t identity, VulkanCommandRetention &retention) {
    const auto found = impl_->targets.find(identity);
    if (found == impl_->targets.end() || !found->second->color || retention.resources.size() == retention.resources.capacity())
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

/** @copydoc VulkanResourceRuntime::CaptureMaterialMesh */
Result<VulkanMaterialMesh> VulkanResourceRuntime::CaptureMaterialMesh(const std::uint64_t identity, VulkanCommandRetention &retention) {
    const auto found = impl_->meshes.find(identity);
    if (found == impl_->meshes.end() || retention.resources.capacity() - retention.resources.size() < 3)
        return Result<VulkanMaterialMesh>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
    const auto vertex = impl_->buffers.find(found->second->vertex);
    const auto index = impl_->buffers.find(found->second->index);
    if (vertex == impl_->buffers.end() || index == impl_->buffers.end())
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
