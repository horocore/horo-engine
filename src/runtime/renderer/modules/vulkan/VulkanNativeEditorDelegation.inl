/** @copydoc IVulkanEditorGraphicsPort::QueryDevice */
Result<Detail::VulkanEditorDevice> QueryDevice() const override {
    if (editorOwner_ != std::this_thread::get_id())
        return Result<Detail::VulkanEditorDevice>::Failure(MakeError(VulkanBackendErrors::WrongThread));
    if (!presentation_ || !device_)
        return Result<Detail::VulkanEditorDevice>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
    const auto format = presentation_->OutputFormat();
    if (format.HasError())
        return Result<Detail::VulkanEditorDevice>::Failure(format.ErrorValue());
    return Result<Detail::VulkanEditorDevice>::Success(
        {instance_,
         editorPhysical_,
         device_,
         graphicsQueue_,
         editorGraphicsFamily_,
         editorFrames_,
         format.Value(),
         dispatch_.getInstanceProcAddr,
         dispatch_.getDeviceProcAddr,
         editorGraphicsIdentity_,
         {enabledGraphics_.api, enabledGraphics_.instanceExtensions, enabledGraphics_.deviceExtensions}});
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
    const Detail::VulkanGuiOutput output{presentationFrame_.image,          presentationFrame_.view,
                                         presentationFrame_.renderFinished, presentationFrame_.extent,
                                         presentationOutput_.clearColor,    !presentationOutput_.outputColor.has_value()};
    const auto recorded = graph_.ExecuteGui(request, output, encoder);
    if (recorded.HasValue())
        presentationCompletion_ = request.submission->signal;
    return recorded;
}

/** @copydoc IVulkanEditorGraphicsPort::WriteFrameBuffer */
Result<void> WriteFrameBuffer(const FrameToken frame, const std::uint64_t instance, const std::span<const std::byte> bytes) override {
    return graph_.WriteFrameBuffer(frame, instance, bytes);
}

/** @copydoc IVulkanEditorGraphicsPort::ObservePreparationBuffer */
Result<bool> ObservePreparationBuffer(const std::uint64_t instance) override {
    return graph_.ObservePreparationBuffer(instance);
}

bool SupportsStereoCoverageReadback() const noexcept override {
    return graph_.SupportsStereoCoverageReadback();
}

Result<bool> ReadCompletedStereoCoverage(std::uint64_t instance, RenderTimelinePoint completion,
                                         std::span<std::byte> destination) override {
    return graph_.ReadCompletedStereoCoverage(instance, completion, destination);
}

/** @copydoc IVulkanEditorGraphicsPort::SupportsStereoCoverageReduction */
bool SupportsStereoCoverageReduction(RenderQueueId queue) const noexcept override {
    return graph_.SupportsStereoCoverageReduction(queue);
}

/** @copydoc IVulkanEditorGraphicsPort::PrepareStereoCoverageReduction */
Result<std::shared_ptr<IResidentStereoCoverageReducer>> PrepareStereoCoverageReduction(
    const Detail::VulkanStereoCoveragePreparation &request) override {
    return graph_.PrepareStereoCoverageReduction(request);
}

/** @copydoc IVulkanEditorGraphicsPort::ReadCompletedStereoCoverageReduction */
Result<bool> ReadCompletedStereoCoverageReduction(std::uint64_t instance, RenderTimelinePoint completion,
                                                  RenderGraphStereoCoverageReduction::Tallies &destination) override {
    return graph_.ReadCompletedStereoCoverageReduction(instance, completion, destination);
}

/** @copydoc IVulkanEditorGraphicsPort::WritePreparationUniform */
Result<void> WritePreparationUniform(const std::uint64_t instance, const std::span<const std::byte> bytes) override {
    return graph_.WritePreparationUniform(instance, bytes);
}

/** @copydoc IVulkanEditorGraphicsPort::WasSubmissionAccepted */
Result<bool> WasSubmissionAccepted(RenderTimelinePoint signal, std::uint64_t deviceIncarnation) const override {
    return graph_.WasSubmissionAccepted(signal, deviceIncarnation);
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
