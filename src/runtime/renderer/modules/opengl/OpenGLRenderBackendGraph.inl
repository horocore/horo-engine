/** @brief Graph admission and completion handoff kept with the private OpenGL backend definition. */
/** @copydoc IRenderBackend::ExecuteGraph */
Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
    if (!IsOwnerThread())
        return WrongThread<void>();
    if (const auto state = ValidateActiveFrame(request.frame); state.HasError())
        return state;
    if (!request.submissions.empty())
        return Result<void>::Failure(
            MakeError(OpenGLBackendErrors::UnsupportedResourceOperation, "OpenGL explicit graph submissions are not implemented."));
    if (!capabilities_.supportsExactTransientResourceReuse)
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    if (vfxPublicationActive_)
        return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
    const auto borrowsNativeState = std::ranges::any_of(request.workloads, [](const auto &binding) {
        return std::holds_alternative<RenderGraphVfx>(binding.workload) ||
               std::holds_alternative<RenderGraphDepthAttachment>(binding.workload);
    });
    // Reuse the existing synchronous native borrow guard: shutdown closes admission, but cannot destroy borrowed metadata.
    const auto clearBorrow = [this, borrowsNativeState](const RenderGraphExecutionRequest *) noexcept {
        if (borrowsNativeState)
            ReleaseVfxNativeBorrow();
    };
    std::unique_ptr<const RenderGraphExecutionRequest, decltype(clearBorrow)> borrow{&request, clearBorrow};
    if (borrowsNativeState)
        vfxPublicationActive_ = true;
    const Detail::OpenGLGraphResources resources{bufferDescriptors_,
                                                 textureDescriptors_,
                                                 &prepared_,
                                                 this,
                                                 capabilities_.support.deviceIncarnation,
                                                 &initialized_};
    if (const auto valid = Detail::ValidateOpenGLGraphWorkloads(request, resources, functions_); valid.HasError())
        return valid;
    if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
        return current;
    if (const auto active = ValidateActiveFrame(request.frame); active.HasError())
        return active;
    if (const auto valid = Detail::ValidateOpenGLGraphWorkloads(request, resources, functions_); valid.HasError())
        return valid;
    if (request.lease != nullptr) {
        if (!execution_.RetainFrameLease(activeFrameSlot_, *request.lease))
            return SynchronizationFailure<void>();
        request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
    }
    // Cover every canonical graph buffer use, including future uniform consumers, under these same fences.
    for (std::size_t index = 0; index < request.resources.size(); ++index) {
        const auto &resource = request.resources[index];
        if (request.graph.Resources()[index].kind == RenderGraphResourceKind::Buffer &&
            resource.instance <= std::numeric_limits<std::uint32_t>::max())
            if (auto use = prepared_.uniformFrameUses.find(static_cast<std::uint32_t>(resource.instance));
                use != prepared_.uniformFrameUses.end())
                use->second.frames |= static_cast<std::uint8_t>(1U << activeFrameSlot_);
    }
    const auto encoded = Detail::ExecuteOpenGLGraphWorkloads(request, resources, functions_, activeExtent_);
    if (encoded.HasError())
        AbortActiveFrame();
    return encoded;
}
