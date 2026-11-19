/** @copydoc VulkanResourceRuntime::UpdateUiPaintFrame */
Result<void> VulkanResourceRuntime::UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &native, const bool &closing) {
    if (closing || !impl_->device)
        return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
    if (const auto valid = ValidateUiPaintFrameUpdate(native.update); valid.HasError())
        return valid;
    const auto &update = native.update;
    const std::array required{update.vertices.size_bytes(), update.indices.size_bytes(),
                              update.parameters.size() * UiPaintFrameUpdate::ParameterStride};
    constexpr std::array usages{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
    std::array<std::shared_ptr<Impl::Buffer>, 3> buffers;
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        const auto existing = impl_->buffers.find(native.instances[index]);
        const auto prior = std::span{native.instances}.first(index);
        if (existing == impl_->buffers.end() || std::ranges::find(prior, native.instances[index]) != prior.end())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto &buffer = existing->second;
        const auto &descriptor = buffer->descriptor;
        if (descriptor.access != RenderBufferAccess::HostVisible || descriptor.usage != usages[index] ||
            required[index] > descriptor.byteSize || update.revision <= buffer->lastUiPaintRevision || buffer->coverageInputSealed)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        buffers[index] = buffer;
    }
    if (update.cancellation.IsCancellationRequested())
        return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
    for (auto &buffer : buffers)
        if (const auto mapped = impl_->MapCoherentBuffer(*buffer); mapped.HasError())
            return mapped;
    // Driver dispatch has returned. Verify the captured residents are still current and the owner has not closed.
    for (std::size_t index = 0; index < buffers.size(); ++index)
        if (Impl::Find(impl_->buffers, native.instances[index]) != buffers[index].get() ||
            update.revision <= buffers[index]->lastUiPaintRevision)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
    if (closing)
        return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
    if (update.cancellation.IsCancellationRequested())
        return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));

    // Coherent mapped copies cannot fail; none of the fields below are observable until the owner lane returns.
    std::memcpy(buffers[0]->mapped, update.vertices.data(), update.vertices.size_bytes());
    std::memcpy(buffers[1]->mapped, update.indices.data(), update.indices.size_bytes());
    auto *parameters = static_cast<std::byte *>(buffers[2]->mapped);
    for (std::size_t index = 0; index < update.parameters.size(); ++index)
        std::memcpy(parameters + index * UiPaintFrameUpdate::ParameterStride, &update.parameters[index], sizeof(UiPaintParameters));
    const VulkanUiPaintPublication publication{native.instances, update.revision, static_cast<std::uint32_t>(update.vertices.size()),
                                               static_cast<std::uint32_t>(update.indices.size()),
                                               static_cast<std::uint32_t>(update.parameters.size())};
    for (auto &buffer : buffers) {
        buffer->uiPaint = publication;
        buffer->lastUiPaintRevision = update.revision;
        buffer->graphState.contentsDefined = true;
        buffer->vfxPublished = false;
    }
    return Result<void>::Success();
}

/** @copydoc VulkanResourceRuntime::ReadUiPaintBuffer */
Result<VulkanUiPaintBufferView> VulkanResourceRuntime::ReadUiPaintBuffer(const std::uint64_t instance) const {
    const auto *buffer = Impl::Find(impl_->buffers, instance);
    if (!buffer || !buffer->mapped || buffer->uiPaint.revision == 0)
        return Result<VulkanUiPaintBufferView>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
    return Result<VulkanUiPaintBufferView>::Success(
        {buffer->uiPaint, {static_cast<const std::byte *>(buffer->mapped), buffer->descriptor.byteSize}});
}
