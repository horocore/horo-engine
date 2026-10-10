/** @brief Graph admission and completion handoff kept with the private OpenGL backend definition. */
/** @copydoc IRenderBackend::ExecuteGraph */
Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
    if (!IsOwnerThread())
        return WrongThread<void>();
    if (const auto state = ValidateActiveFrame(request.frame); state.HasError())
        return state;
    if (!capabilities_.supportsExactTransientResourceReuse)
        return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
    const Detail::OpenGLGraphResources resources{bufferDescriptors_, textureDescriptors_};
    if (const auto valid = Detail::ValidateOpenGLGraphWorkloads(request, resources); valid.HasError())
        return valid;
    if (const auto current = presentationPort_->MakeCurrent(); current.HasError())
        return current;
    if (request.lease != nullptr) {
        if (!execution_.RetainFrameLease(activeFrameSlot_, *request.lease))
            return SynchronizationFailure<void>();
        request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
    }
    const auto encoded = Detail::ExecuteOpenGLGraphWorkloads(request, resources, functions_, activeExtent_);
    if (encoded.HasError())
        AbortActiveFrame();
    return encoded;
}
