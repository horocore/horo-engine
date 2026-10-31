/** @brief Prepared material consumer admission and the existing exact frame-completion handoff. */
/** @copydoc IRenderBackend::ExecuteMaterialPass */
Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request) override {
    if (!IsOwnerThread())
        return WrongThread<void>();
    if (const auto active = ValidateActiveFrame(request.frame); active.HasError())
        return active;
    if (!functions_.materialExecution.IsValid() || !functions_.materialExecution.isAvailable())
        return Result<void>::Failure(MakeError(MaterialBindingErrors::Unsupported));
    if (!request.lease || !request.transfer || request.transfer->authority != RenderGraphLeaseAuthority::Frontend || request.target == 0 ||
        request.target > std::numeric_limits<std::uint32_t>::max() || !request.extent.IsValid() || !request.color.IsValid() ||
        request.color.storeOperation != AttachmentStoreOperation::Store ||
        request.color.loadOperation == AttachmentLoadOperation::DontCare || request.draws.empty() ||
        request.draws.size() > RenderMaterialPass::MaximumDraws ||
        (request.depthClear && (!std::isfinite(*request.depthClear) || *request.depthClear < 0 || *request.depthClear > 1)))
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig));
    const auto target = prepared_.materialTargets.find(static_cast<std::uint32_t>(request.target));
    if (target == prepared_.materialTargets.end() || target->second.extent != request.extent || target->second.colorTexture == 0 ||
        (request.depthClear && target->second.depthTexture == 0))
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
    std::array<Detail::OpenGLMaterialDrawCommand, RenderMaterialPass::MaximumDraws> commands;
    for (std::size_t index = 0; index < request.draws.size(); ++index) {
        const auto &draw = request.draws[index];
        const auto *material = dynamic_cast<const Detail::OpenGLMaterialBinding *>(draw.material);
        if (!material || material->owner != this || draw.mesh == 0 || draw.mesh > std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
        const auto mesh = prepared_.meshDescriptors.find(static_cast<std::uint32_t>(draw.mesh));
        const auto program = prepared_.graphicsPipelines.find(material->program);
        if (mesh == prepared_.meshDescriptors.end() || program == prepared_.graphicsPipelines.end() ||
            mesh->second.descriptor.indexCount > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            program->second.state.colorFormat != target->second.colorFormat ||
            program->second.state.depthFormat != target->second.depthFormat ||
            program->second.state.sampleCount != target->second.sampleCount)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
        for (std::size_t slotIndex = 0; slotIndex < material->count; ++slotIndex) {
            const auto &slot = material->slots[slotIndex];
            if (slot.binding >= 128 || slot.object == 0)
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            if (slot.kind == MaterialResidentResourceKind::Buffer) {
                if (!bufferDescriptors_.contains(slot.object))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            } else if (!textures_.contains(slot.object) || !prepared_.samplerDescriptors.contains(slot.sampler) ||
                       slot.object == target->second.colorTexture || slot.object == target->second.depthTexture) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                              "Material sampling references a stale native instance or aliases a current attachment."));
            }
        }
        commands[index] = {material->program,
                           mesh->first,
                           mesh->second.descriptor.indexCount,
                           mesh->second.descriptor.indexFormat,
                           program->second.state,
                           program->second.features,
                           std::span{material->slots}.first(material->count)};
    }
    if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
        return current;
    // Transfer before encoding. Even a partial draw, native failure, or frame cancellation retains every generation.
    if (!execution_.RetainFrameLease(activeFrameSlot_, *request.lease))
        return SynchronizationFailure<void>();
    request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
    // Record every canonical material use, including a non-VFX consumer of a reusable uniform generation.
    // The existing frame fence covers partial encoding and abort; an update never relies on CPU lease counts.
    for (const auto &command : std::span{commands}.first(request.draws.size()))
        for (const auto &slot : command.slots)
            if (slot.kind == MaterialResidentResourceKind::Buffer)
                if (auto use = prepared_.uniformFrameUses.find(slot.object); use != prepared_.uniformFrameUses.end())
                    use->second.frames |= static_cast<std::uint8_t>(1U << activeFrameSlot_);
    const auto encoded = functions_.materialExecution.execute(request, std::span{commands}.first(request.draws.size()));
    if (encoded.HasError())
        AbortActiveFrame();
    return encoded;
}
