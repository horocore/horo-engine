/** @brief Pure current-namespace admission; no map iterator or reference escapes a callback boundary. */
Result<Detail::OpenGLPreparedMaterialState::UniformFrameUse> VfxUpdateState(const NativeVfxFrameUpdate &update) const {
    using State = Result<Detail::OpenGLPreparedMaterialState::UniformFrameUse>;
    if (!initialized_ || closing_ || frameActive_ || update.deviceIncarnation == 0 ||
        update.deviceIncarnation != capabilities_.support.deviceIncarnation)
        return State::Failure(MakeError(VfxFrameErrors::Unavailable));
    const auto object = static_cast<std::uint32_t>(update.instance);
    const auto descriptor = bufferDescriptors_.find(object);
    const auto use = prepared_.uniformFrameUses.find(object);
    const auto *material = dynamic_cast<const Detail::OpenGLMaterialBinding *>(update.material);
    const auto pipeline = prepared_.graphicsPipelines.find(static_cast<std::uint32_t>(update.pipeline));
    if (!material || material->owner != this || material->program != update.pipeline ||
        material->deviceIncarnation != update.deviceIncarnation || pipeline == prepared_.graphicsPipelines.end() ||
        !Detail::SameMaterialLayout(pipeline->second.layout, *update.layout) || VfxShaderParameterBytes(*update.layout).HasError() ||
        descriptor == bufferDescriptors_.end() || use == prepared_.uniformFrameUses.end() ||
        use->second.deviceIncarnation != update.deviceIncarnation || descriptor->second.usage != RenderBufferUsage::Uniform ||
        descriptor->second.access != RenderBufferAccess::HostVisible || update.parameters.size() != descriptor->second.byteSize ||
        update.revision <= use->second.revision || (use->second.revision && update.parameters.size() != use->second.blockBytes))
        return State::Failure(MakeError(VfxFrameErrors::InvalidInput));
    const auto mapping = std::ranges::find_if(update.layout->targetBindings, [](const auto &entry) {
        return entry.id == VfxShaderParameterIds::Instances && entry.active && entry.generatedHelperIndex == 0;
    });
    if (mapping == update.layout->targetBindings.end())
        return State::Failure(MakeError(VfxFrameErrors::InvalidInput));
    bool matchingRange = false;
    for (const auto &slot : std::span{material->slots}.first(material->count)) {
        if (slot.kind != MaterialResidentResourceKind::Buffer || slot.object != object)
            continue;
        if (matchingRange || slot.binding != mapping->nativeBinding || slot.offset != 0 || slot.byteCount != update.parameters.size())
            return State::Failure(MakeError(VfxFrameErrors::InvalidInput));
        matchingRange = true;
    }
    return matchingRange ? State::Success(use->second) : State::Failure(MakeError(VfxFrameErrors::InvalidInput));
}

/** @brief Invalidates failed native publication without retaining mutable metadata across callbacks. */
void InvalidateVfxPublication(const std::uint32_t object) noexcept {
    if (auto use = prepared_.uniformFrameUses.find(object); use != prepared_.uniformFrameUses.end())
        use->second.published = false;
}

/** @copydoc IRenderBackend::UpdateVfxFrame */
Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &update) override {
    if (!IsOwnerThread())
        return WrongThread<void>();
    if (vfxPublicationActive_)
        return Result<void>::Failure(MakeError(VfxFrameErrors::Pending, "A VFX publication already owns this native safe point."));
    if (!functions_.buffers.bufferSubData || !functions_.materials.IsValid())
        return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
    if (!update.writeStatus || *update.writeStatus != VfxFrameWriteStatus::NotStarted || !update.layout || update.instance == 0 ||
        update.instance > std::numeric_limits<std::uint32_t>::max() || update.pipeline == 0 ||
        update.pipeline > std::numeric_limits<std::uint32_t>::max() || update.revision == 0 || update.instanceCount == 0 ||
        update.instanceCount > MaximumVfxDrawInstances)
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    if (const auto state = VfxUpdateState(update); state.HasError())
        return Result<void>::Failure(state.ErrorValue());
    vfxPublicationActive_ = true;
    const auto releaseMutation = [this](const NativeVfxFrameUpdate *) noexcept {
        ReleaseVfxNativeBorrow();
    };
    std::unique_ptr<const NativeVfxFrameUpdate, decltype(releaseMutation)> mutation{&update, releaseMutation};
    if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
        return current;
    if (const auto state = VfxUpdateState(update); state.HasError())
        return Result<void>::Failure(state.ErrorValue());
    // The frontend retains this immutable descriptor, unlike mutable backend map nodes.
    const auto mapping = std::ranges::find_if(update.layout->targetBindings, [](const auto &entry) {
        return entry.id == VfxShaderParameterIds::Instances && entry.active && entry.generatedHelperIndex == 0;
    });
    const auto nativeBytes = functions_.materials.uniformBlockSize(static_cast<std::uint32_t>(update.pipeline), mapping->nativeName);
    if (nativeBytes.HasError())
        return Result<void>::Failure(nativeBytes.ErrorValue());
    const auto beforePoll = VfxUpdateState(update);
    if (beforePoll.HasError())
        return Result<void>::Failure(beforePoll.ErrorValue());
    if (nativeBytes.Value() != update.parameters.size())
        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
    const auto retired = execution_.AreFrameSlotsRetired(beforePoll.Value().frames);
    if (retired.HasError())
        return Result<void>::Failure(retired.ErrorValue());
    if (!retired.Value())
        return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
    if (const auto state = VfxUpdateState(update); state.HasError())
        return Result<void>::Failure(state.ErrorValue());
    if (update.cancellation.IsCancellationRequested())
        return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
    if (functions_.state.error() != GL_NO_ERROR)
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
    std::array<std::int32_t, 1> previous{};
    functions_.state.getInteger(GL_ARRAY_BUFFER_BINDING, previous);
    if (functions_.state.error() != GL_NO_ERROR)
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
    const auto object = static_cast<std::uint32_t>(update.instance);
    functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, object);
    if (const auto state = VfxUpdateState(update); state.HasError()) {
        if (contextCreated_ && !drained_)
            functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(previous[0]));
        return Result<void>::Failure(state.ErrorValue());
    }
    *update.writeStatus = VfxFrameWriteStatus::Attempted;
    functions_.buffers.bufferSubData(GL_ARRAY_BUFFER, update.parameters);
    const auto written = functions_.state.error();
    if (contextCreated_ && !drained_)
        functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(previous[0]));
    const auto restored = functions_.state.error();
    const auto afterWrite = VfxUpdateState(update);
    if (afterWrite.HasError() || written != GL_NO_ERROR || restored != GL_NO_ERROR) {
        InvalidateVfxPublication(object);
        return afterWrite.HasError() ? Result<void>::Failure(afterWrite.ErrorValue())
                                     : Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
    }
    // Fresh lookup after every potentially reentrant native operation; no old iterator is dereferenced.
    prepared_.uniformFrameUses.at(object) = {.revision = update.revision,
                                             .blockBytes = update.parameters.size(),
                                             .instanceCount = update.instanceCount,
                                             .published = true,
                                             .deviceIncarnation = update.deviceIncarnation};
    *update.writeStatus = VfxFrameWriteStatus::Committed;
    return Result<void>::Success();
}
