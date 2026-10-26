/** @copydoc VulkanResourceRuntime::ImportExternalTexture */
Result<std::uint64_t> VulkanResourceRuntime::ImportExternalTexture(const IRenderExternalTextureSource &source,
                                                                   RenderResourceNativeLifetime lifetime) {
    const auto *native = dynamic_cast<const IVulkanExternalTextureSource *>(&source);
    if (native == nullptr || impl_->device == VK_NULL_HANDLE || native->NativeDevice() != impl_->device ||
        native->NativeImage() == VK_NULL_HANDLE || !source.Descriptor().IsValid() || !lifetime.HasLease())
        return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceIdentityInvalid,
                                                            "External texture requires the exact device and canonical native lifetime."));
    if (auto current = source.Validate(); current.HasError())
        return Result<std::uint64_t>::Failure(current.ErrorValue());
    for (const auto &[id, resident] : impl_->textures) {
        (void)id;
        if (resident->handle == native->NativeImage())
            return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceIdentityInvalid,
                                                                "This runtime image already has a live renderer registration."));
    }
    const auto &descriptor = source.Descriptor();
    const auto acquired = native->AcquiredState();
    const auto final = source.RequiredFinalState();
    const bool depth = acquired.operation == RenderGraphSynchronizationOperation::DepthStencilAttachment;
    const bool depthFormat =
        descriptor.format == RenderTextureFormat::Depth16Unorm || descriptor.format == RenderTextureFormat::Depth24Stencil8 ||
        descriptor.format == RenderTextureFormat::Depth32Float || descriptor.format == RenderTextureFormat::Depth32FloatStencil8;
    if (depthFormat != depth || descriptor.dimension != RenderTextureDimension::TwoD || descriptor.mipCount != 1 ||
        descriptor.sampleCount != 1 || !HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) ||
        acquired.scope != RenderGraphPipelineScope::Graphics || !acquired.queue.IsValid() ||
        acquired.access != RenderGraphSynchronizationAccess::ReadWrite ||
        (acquired.operation != RenderGraphSynchronizationOperation::ColorAttachment && !depth) ||
        acquired.layout != (depth ? RenderGraphTextureLayout::DepthStencilAttachment : RenderGraphTextureLayout::ColorAttachment) ||
        final.scope != RenderGraphPipelineScope::External || final.queue != acquired.queue || final.layout != acquired.layout ||
        final.operation != acquired.operation || final.access != acquired.access)
        return Result<std::uint64_t>::Failure(
            ResourceError(VulkanBackendErrors::ResourceUnsupported,
                          "External Vulkan image acquisition/final state is not the qualified attachment tuple."));
    auto texture = impl_->AllocateExternal<Impl::Texture>();
    if (!texture)
        return Result<std::uint64_t>::Failure(
            ResourceError(VulkanBackendErrors::ResourceCreationFailed, "External texture registration storage is unavailable."));
    texture->externalSource = native;
    texture->device = impl_->device;
    texture->handle = native->NativeImage();
    texture->borrowed = true;
    texture->descriptor = descriptor;
    texture->graphState = {.logical = acquired, .hasNativeState = true, .contentsDefined = false};
    texture->lifetime = std::move(lifetime);
    texture->destroyView = impl_->dispatch.destroyImageView;
    const VkImageAspectFlags aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = texture->handle,
                                     .viewType = descriptor.layerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                     .format = VulkanResourceConversion::TextureFormat(descriptor.format),
                                     .subresourceRange = {.aspectMask = aspectMask,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = descriptor.layerCount}};
    const auto prepared = native->PreparedNativeViews();
    if (prepared.attachment != VK_NULL_HANDLE) {
        texture->colorView = prepared.attachment;
        texture->borrowedView = true;
    } else if (impl_->dispatch.createImageView(impl_->device, &view, nullptr, &texture->colorView) != VK_SUCCESS) {
        texture->colorView = VK_NULL_HANDLE;
        return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceCreationFailed,
                                                            "Runtime image view creation failed; the foreign allocation is preserved."));
    }
    return impl_->RegisterExternal(impl_->textures, impl_->external.textures, std::move(texture));
}

/** @copydoc VulkanResourceRuntime::ExternalTextureSource */
const IVulkanExternalTextureSource *VulkanResourceRuntime::ExternalTextureSource(const std::uint64_t instance) const noexcept {
    const auto *texture = Impl::Find(impl_->textures, instance);
    return texture != nullptr ? texture->externalSource : nullptr;
}
