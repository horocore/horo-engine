/** @copydoc IVulkanEditorGraphicsPort::QueryDevice */
Result<Detail::VulkanEditorDevice> QueryDevice() const override {
    if (editorOwner_ != std::this_thread::get_id())
        return Result<Detail::VulkanEditorDevice>::Failure(MakeError(VulkanBackendErrors::WrongThread));
    if (!presentation_ || !device_)
        return Result<Detail::VulkanEditorDevice>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
    const auto format = presentation_->OutputFormat();
    if (format.HasError())
        return Result<Detail::VulkanEditorDevice>::Failure(format.ErrorValue());
    return Result<Detail::VulkanEditorDevice>::Success({instance_, editorPhysical_, device_, graphicsQueue_, editorGraphicsFamily_,
                                                        editorFrames_, format.Value(), dispatch_.getInstanceProcAddr,
                                                        dispatch_.getDeviceProcAddr, editorGraphicsIdentity_});
}

/** @copydoc IVulkanEditorGraphicsPort::FrameSlot */
Result<Detail::VulkanFrameRetentionSlot> FrameSlot(const FrameToken frame) const override {
    return graph_.FrameSlot(frame);
}

/** @copydoc IVulkanCommandPort::PrepareGuiComposition */
Result<void> PrepareGuiComposition(const GuiCompositionRequest &request) override {
    if (!presentation_ || !primaryOutputPrepared_ || request.frame != presentationFrameToken_)
        return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
    return graph_.PrepareGuiComposition(request);
}

/** @copydoc IVulkanEditorGraphicsPort::ExecuteGui */
Result<void> ExecuteGui(const GuiCompositionRequest &request, Detail::IVulkanGuiEncoder &encoder) override {
    if (!presentation_ || !primaryOutputPrepared_ || request.frame != presentationFrameToken_)
        return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
    const Detail::VulkanGuiOutput output{presentationFrame_.image, presentationFrame_.view, presentationFrame_.renderFinished,
                                         presentationFrame_.extent, presentationOutput_.clearColor};
    const auto recorded = graph_.ExecuteGui(request, output, encoder);
    if (recorded.HasValue())
        presentationCompletion_ = request.submission->signal;
    return recorded;
}

/** @copydoc IVulkanEditorGraphicsPort::WriteFrameBuffer */
Result<void> WriteFrameBuffer(const FrameToken frame, const std::uint64_t instance, const std::span<const std::byte> bytes) override {
    if (!graph_.CanWriteFrameInputs(frame))
        return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
    const auto completion = resources_.BufferCompletion(instance);
    if (completion.HasError())
        return Result<void>::Failure(completion.ErrorValue());
    if (completion.Value().IsValid()) {
        const auto retired = graph_.ObserveSubmission(
            {.completion = completion.Value(), .expectedDeviceIncarnation = resourceCapabilities_.deviceIncarnation});
        if (retired.HasError())
            return Result<void>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
    }
    return resources_.WriteHostFrameBuffer(instance, bytes);
}

/** @copydoc IVulkanEditorGraphicsPort::AttachDeviceChild */
Result<void> AttachDeviceChild(Detail::IVulkanEditorDeviceChild &child) override {
    if (editorOwner_ != std::this_thread::get_id())
        return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
    if (!device_ || std::ranges::find(editorChildren_, &child) != editorChildren_.end())
        return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
    const auto free = std::ranges::find(editorChildren_, nullptr);
    if (free == editorChildren_.end())
        return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
    *free = &child;
    return Result<void>::Success();
}

/** @copydoc IVulkanEditorGraphicsPort::DetachDeviceChild */
void DetachDeviceChild(Detail::IVulkanEditorDeviceChild &child) noexcept override {
    if (editorOwner_ != std::this_thread::get_id())
        std::terminate();
    const auto found = std::ranges::find(editorChildren_, &child);
    if (found != editorChildren_.end())
        *found = nullptr;
}

/** @copydoc IVulkanEditorGraphicsPort::BorrowSample */
Result<Detail::VulkanGuiSampleBinding> BorrowSample(const GuiSampledResourceInstance &sample) const override {
    if (const auto device = QueryDevice(); device.HasError())
        return Result<Detail::VulkanGuiSampleBinding>::Failure(device.ErrorValue());
    Detail::VulkanCommandRetention temporary;
    temporary.resources.reserve(3);
    const auto captured = resources_.CaptureGuiSample(sample, temporary);
    if (captured.HasError())
        return Result<Detail::VulkanGuiSampleBinding>::Failure(captured.ErrorValue());
    return Result<Detail::VulkanGuiSampleBinding>::Success({captured.Value().view, captured.Value().sampler});
}

/** @copydoc IVulkanEditorGraphicsPort::PreparePrimaryOutput */
Result<void> PreparePrimaryOutput(const FrameToken frame, const PrimaryOutputAttachment attachment) override {
    if (const auto slot = graph_.FrameSlot(frame); slot.HasError())
        return Result<void>::Failure(slot.ErrorValue());
    if (attachment.outputColor)
        return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
    if (!presentation_ || frame != presentationFrameToken_ || primaryOutputPrepared_ || !attachment.IsValid() ||
        attachment.loadOperation != AttachmentLoadOperation::Clear || attachment.storeOperation != AttachmentStoreOperation::Store)
        return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
    presentationOutput_ = attachment;
    primaryOutputPrepared_ = true;
    return Result<void>::Success();
}
