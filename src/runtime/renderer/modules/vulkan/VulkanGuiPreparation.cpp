#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    /** @copydoc VulkanGraphRuntime::Impl::PendingState */
    const VulkanGraphResourceState &VulkanGraphRuntime::Impl::PendingState(const VulkanGraphResource &resource) const {
        for (auto update = updates.rbegin(); update != updates.rend(); ++update)
            if (update->target == resource.state)
                return update->value;
        return *resource.state;
    }

    /** @copydoc VulkanGraphRuntime::Impl::GuiState */
    Result<void> VulkanGraphRuntime::Impl::GuiState(const VulkanGraphResource &resource, const RenderGraphLogicalState next,
                                                    const RenderQueueSubmission &submission, const bool requireContents) {
        if (resource.state == nullptr || barriers.size() == barriers.capacity() || updates.size() == updates.capacity())
            return Result<void>::Failure(Invalid("GUI state storage or exact native backing is unavailable."));
        const auto before = PendingState(resource);
        if ((requireContents && !before.contentsDefined) || (before.hasNativeState && before.logical.queue != submission.queue))
            return Result<void>::Failure(Invalid("GUI sample/input requires defined contents and explicit graphics queue ownership."));
        const auto barrier =
            TranslateVulkanGraphBarrier(resource, before.hasNativeState ? before.logical : RenderGraphLogicalState{}, next);
        if (barrier.HasError())
            return Result<void>::Failure(barrier.ErrorValue());
        barriers.push_back(barrier.Value());
        updates.push_back({resource.state, {next, submission.signal, true, true}});
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::PrepareGuiComposition */
    Result<void> VulkanGraphRuntime::PrepareGuiComposition(const GuiCompositionRequest &request) {
        if (const auto access = impl_->Access(); access.HasError())
            return access;
        auto *pins = impl_->lifecycle.pins;
        if (request.frame != impl_->lifecycle.frame || !request.frame.IsValid() || pins == nullptr || impl_->guiPrepared ||
            request.mode != GuiCompositionMode::ExplicitGraphicsSubmission || !request.submission || !request.lease || !request.transfer ||
            request.resources.size() > GuiCompositionPreparation::MaximumResources ||
            request.buffers.size() > GuiCompositionPreparation::MaximumResources ||
            request.submission->waits.size() > RenderGraphSubmissionLimits::MaximumQueues ||
            (pins->lease != nullptr && pins->lease != request.lease) || (request.extendsExistingLease != (pins->lease != nullptr)))
            return Result<void>::Failure(
                Impl::Invalid("GUI admission requires the exact current frame, canonical lease and bounded resources."));
        const auto *queue = impl_->Queue(request.submission->queue);
        if (!queue || !queue->supportsGraphics || !impl_->native.copyImage)
            return Result<void>::Failure(Impl::Unsupported("GUI requires admitted graphics and Vulkan1.3 image-copy dispatch."));
        std::array<RenderGraphQueueSubmission, RenderGraphSubmissionLimits::MaximumSubmissions> records;
        if (impl_->submissions.size() == records.size())
            return Result<void>::Failure(Impl::Invalid("GUI tail exceeds bounded submission storage."));
        for (std::size_t index = 0; index < impl_->submissions.size(); ++index)
            records[index] = {impl_->submissions[index].value, {index, 1}};
        records[impl_->submissions.size()] = {*request.submission, {impl_->submissions.size(), 1}};
        if (const auto valid = impl_->commands.ValidateBatch(std::span{records}.first(impl_->submissions.size() + 1)); valid.HasError())
            return valid;
        impl_->guiSubmission = *request.submission;
        std::ranges::copy(request.submission->waits, impl_->guiWaits.begin());
        impl_->guiSubmission->waits = std::span{impl_->guiWaits}.first(request.submission->waits.size());
        std::ranges::copy(request.resources, impl_->guiIdentities.begin());
        std::ranges::copy(request.buffers, impl_->guiBufferIdentities.begin());
        impl_->guiSampleCount = request.resources.size();
        impl_->guiBufferCount = request.buffers.size();
        pins->lease = request.lease;
        request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
        impl_->guiPrepared = true;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::PrepareGuiBuffers */
    Result<void> VulkanGraphRuntime::Impl::PrepareGuiBuffers(const GuiCompositionRequest &request) {
        auto &pins = *lifecycle.pins;
        const auto &submission = *request.submission;
        const RenderGraphLogicalState input{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::GraphicsInputBuffer, RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::NotApplicable, submission.queue};
        guiBufferBarriers = {barriers.size(), 0};
        for (std::size_t index = 0; index < request.buffers.size(); ++index) {
            const auto captured = resources->CaptureGuiBuffer(request.buffers[index], pins);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            guiBuffers[index] = captured.Value();
            guiNativeBuffers[index] = {captured.Value().resource.buffer, captured.Value().memory,
                                       captured.Value().resource.bufferDescriptor.byteSize};
            if (const auto state = GuiState(guiBuffers[index].resource, input, submission, true); state.HasError())
                return state;
        }
        guiBufferBarriers.count = barriers.size() - guiBufferBarriers.offset;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Impl::PrepareGuiResources */
    Result<void> VulkanGraphRuntime::Impl::PrepareGuiResources(const GuiCompositionRequest &request) {
        if (const auto buffers = PrepareGuiBuffers(request); buffers.HasError())
            return buffers;
        auto &pins = *lifecycle.pins;
        const auto &submission = *request.submission;
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto captured = resources->CaptureGuiSample(request.resources[index], pins);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            guiSamples[index] = captured.Value();
            guiSampleBindings[index] = {captured.Value().view, captured.Value().sampler};
            auto &plan = guiPlans[index];
            const auto &sample = guiSamples[index];
            plan = {{barriers.size(), 0}, {}, !PendingState(sample.texture).contentsDefined};
            if (plan.upload) {
                if (!sample.staging.buffer)
                    return Result<void>::Failure(Invalid("Undefined GUI sample requires an exact same-tail staging generation."));
                const RenderGraphLogicalState source{RenderGraphSynchronizationAccess::Read,
                                                     RenderGraphSynchronizationOperation::CopySource, RenderGraphPipelineScope::Transfer,
                                                     RenderGraphTextureLayout::NotApplicable, submission.queue};
                const RenderGraphLogicalState destination{RenderGraphSynchronizationAccess::Write,
                                                          RenderGraphSynchronizationOperation::CopyDestination,
                                                          RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::CopyDestination,
                                                          submission.queue};
                if (const auto state = GuiState(sample.staging, source, submission, true); state.HasError())
                    return state;
                if (const auto state = GuiState(sample.texture, destination, submission, false); state.HasError())
                    return state;
            }
            plan.before.count = barriers.size() - plan.before.offset;
            plan.after = {barriers.size(), 0};
            const RenderGraphLogicalState sampled{RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Sampled,
                                                  RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::ShaderReadOnly,
                                                  submission.queue};
            if (const auto state = GuiState(sample.texture, sampled, submission, true); state.HasError())
                return state;
            plan.after.count = barriers.size() - plan.after.offset;
        }
        // SYNC: Restore every sampled runtime image only AFTER all GUI draws, in this same canonical tail submission.
        guiExternalFinalBarriers = {barriers.size(), 0};
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto &sample = guiSamples[index];
            if (sample.requiredFinal) {
                if (sample.requiredFinal->queue != submission.queue)
                    return Result<void>::Failure(Invalid("XR GUI mirror requires the exact acquired graphics queue."));
                if (const auto state = GuiState(sample.texture, *sample.requiredFinal, submission, true); state.HasError())
                    return state;
            }
        }
        guiExternalFinalBarriers.count = barriers.size() - guiExternalFinalBarriers.offset;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
