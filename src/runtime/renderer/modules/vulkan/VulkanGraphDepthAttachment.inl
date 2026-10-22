/** @brief Validates the bounded depth workload against the actual registered native image and queue. */
Result<void> VulkanGraphRuntime::Impl::ValidateDepthAttachment(const RenderGraphDepthAttachment &depth, const RenderQueueId queue) {
    const auto &binding = bindings[depth.texture.value - 1];
    const auto *nativeQueue = Queue(queue);
    if (nativeQueue == nullptr || !nativeQueue->supportsGraphics || binding.colorView == VK_NULL_HANDLE ||
        binding.kind != RenderGraphResourceKind::Texture || binding.textureDescriptor.sampleCount != 1 ||
        (binding.textureDescriptor.format != RenderTextureFormat::Depth16Unorm &&
         binding.textureDescriptor.format != RenderTextureFormat::Depth32Float))
        return Result<void>::Failure(Unsupported("Depth work requires the exact admitted single-sample native depth attachment."));
    states[depth.texture.value - 1].contentsDefined = true;
    return Result<void>::Success();
}

/** @brief Emits a normalized depth clear for every array slice of the actual registered runtime allocation. */
void VulkanGraphRuntime::Impl::EncodeDepthAttachment(const VkCommandBuffer command,
                                                     const RenderGraphDepthAttachment &depth) const noexcept {
    const auto &binding = bindings[depth.texture.value - 1];
    const VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView = binding.colorView,
                                               .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .clearValue = {.depthStencil = {depth.clearDepth, 0}}};
    const VkRenderingInfo operation{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0},
                                                   .extent = {binding.textureDescriptor.extent.width,
                                                              binding.textureDescriptor.extent.height}},
                                    .layerCount = binding.textureDescriptor.layerCount,
                                    .pDepthAttachment = &attachment};
    native.beginRendering(command, &operation);
    native.endRendering(command);
}
