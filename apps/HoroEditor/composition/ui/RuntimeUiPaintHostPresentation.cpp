#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    /** @copydoc Detail::RuntimeUiPaintHostState::ActiveSource */
    Detail::RuntimeUiPaintSource *Detail::RuntimeUiPaintHostState::ActiveSource(const RuntimeUiInstanceId instance) noexcept {
        if (!Available() || dispatching || phase != RuntimeUiPaintHostPhase::Active)
            return nullptr;
        const auto sources = Sources();
        const auto found = std::ranges::find_if(sources, [instance](const auto *entry) {
            return entry && entry->instance == instance;
        });
        return found == sources.end() ? nullptr : *found;
    }

    /** @copydoc Detail::RuntimeUiPaintSource::FindSnapshot */
    Detail::RuntimeUiPaintSource::SnapshotView *Detail::RuntimeUiPaintSource::FindSnapshot(
        const UiPresentationAttachmentId attachment) noexcept {
        const auto found = std::ranges::find_if(snapshotViews, [attachment](const auto &entry) {
            return entry.projection && entry.attachment == attachment;
        });
        return found == snapshotViews.end() ? nullptr : &*found;
    }

    /** @copydoc RuntimeUiPaintPort::AttachSnapshotView */
    Result<UiPresentationAttachmentId> RuntimeUiPaintPort::AttachSnapshotView(const RuntimeUiInstanceId instance, const UiRenderViewId view,
                                                                              UiAnimationViewport viewport) const {
        using Attached = Result<UiPresentationAttachmentId>;
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source)
            return Attached::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto slot = std::ranges::find_if(source->snapshotViews, [](const auto &entry) {
            return !entry.projection;
        });
        if (slot == source->snapshotViews.end())
            return Attached::Failure(MakeError(UiErrors::CapacityExceeded));
        auto projection = source->projection->ForkForView(state_->service, view);
        if (projection.HasError())
            return Attached::Failure(projection.ErrorValue());
        auto attached = source->animation->Attach(view, std::move(viewport));
        if (attached.HasValue())
            *slot = {attached.Value(), std::move(projection).Value()};
        return attached;
    }

    /** @copydoc RuntimeUiPaintPort::UpdateSnapshotView */
    Result<void> RuntimeUiPaintPort::UpdateSnapshotView(const RuntimeUiInstanceId instance, const UiPresentationAttachmentId attachment,
                                                        UiAnimationViewport viewport) const {
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!source->FindSnapshot(attachment))
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        return source->animation->UpdateAttachment(attachment, std::move(viewport));
    }

    /** @copydoc RuntimeUiPaintPort::DetachSnapshotView */
    Result<void> RuntimeUiPaintPort::DetachSnapshotView(const RuntimeUiInstanceId instance,
                                                        const UiPresentationAttachmentId attachment) const {
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto *view = source->FindSnapshot(attachment);
        if (!view)
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        const auto detached = source->animation->Detach(attachment);
        if (detached.HasValue())
            *view = {};
        return detached;
    }

    /** @copydoc RuntimeUiPaintPort::ExtractSnapshotView */
    Result<UiRenderSnapshot> RuntimeUiPaintPort::ExtractSnapshotView(const RuntimeUiInstanceId instance,
                                                                     const UiPresentationAttachmentId attachment) const {
        using Extracted = Result<UiRenderSnapshot>;
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source)
            return Extracted::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto *view = source->FindSnapshot(attachment);
        if (!view)
            return Extracted::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        auto frame = source->animation->Acquire();
        if (frame.HasError())
            return Extracted::Failure(frame.ErrorValue());
        const auto published = frame.Value().Attachments();
        const auto exact = std::ranges::find(published, attachment, &UiAnimationAttachmentFrame::attachment);
        if (exact == published.end() || !exact->layout)
            return Extracted::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        return view->projection->Extract(state_->service, frame.Value());
    }

    /** @copydoc RuntimeUiPaintPort::AcquireFrame */
    Result<UiAnimationFrameLease> RuntimeUiPaintPort::AcquireFrame() const {
        if (!state_ || !state_->Available() || !state_->source || state_->phase != RuntimeUiPaintHostPhase::Active)
            return Result<UiAnimationFrameLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        return state_->source->animation->Acquire();
    }

    /** @copydoc RuntimeUiPaintPort::InputEligible */
    bool RuntimeUiPaintPort::InputEligible(const UiRenderViewId view) const noexcept {
        if (!state_ || !state_->Available())
            return false;
        return std::ranges::any_of(state_->Sources(), [view](const auto *source) {
            return source && !source->output.pending && !source->output.awaitingPresentation && source->animation->InputEligible(view);
        });
    }

    /** @copydoc RuntimeUiPaintPort::ReadSceneCanvasFrame */
    Result<std::optional<RuntimeUiPaintSceneCanvasFrame>> RuntimeUiPaintPort::ReadSceneCanvasFrame(
        const std::uint64_t frameNumber, const Runtime::SceneRuntimeId scene, const std::optional<UiRenderMode> mode) const {
        using Read = Result<std::optional<RuntimeUiPaintSceneCanvasFrame>>;
        if (!state_ || !state_->Available() || state_->dispatching)
            return Read::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        for (const auto *source : state_->Sources())
            if (source && source->sceneCanvas && source->scene == scene && source->frame.extractedFrame == frameNumber &&
                (!mode || source->output.canvas.renderMode == *mode))
                return Read::Success(RuntimeUiPaintSceneCanvasFrame{source->instance, scene,
                                                                    source->worldCanvas ? &*source->worldCanvas : nullptr,
                                                                    &*source->frame.snapshot, &source->programs.policy,
                                                                    &source->programs.kernel, source->textures});
        return Read::Success(std::nullopt);
    }

    /** @copydoc RuntimeUiPaintPort::TakeSceneCanvasSubmission */
    Result<Render::UiRenderSubmission> RuntimeUiPaintPort::TakeSceneCanvasSubmission(const RuntimeUiInstanceId instance,
                                                                                     const std::uint64_t frameNumber,
                                                                                     const Render::UiPaintProgram &program,
                                                                                     const std::uint32_t slot) const {
        using Submission = Result<Render::UiRenderSubmission>;
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source || !source->sceneCanvas || source->frame.extractedFrame != frameNumber || slot >= source->programs.policy.frameSlots ||
            source->frame.sceneSubmissionTaken)
            return Submission::Failure(MakeError(UiErrors::RevisionStale));
        auto submission = source->BindResources(program, slot);
        if (submission.HasValue())
            source->frame.sceneSubmissionTaken = true;
        return submission;
    }

    /** @copydoc RuntimeUiPaintPort::SceneCanvasEncoded */
    Result<void> RuntimeUiPaintPort::SceneCanvasEncoded(const RuntimeUiInstanceId instance, const std::uint64_t frameNumber,
                                                        const std::optional<UiWorldCanvasProjection> plane) const {
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source || !source->sceneCanvas || source->frame.extractedFrame != frameNumber || !source->frame.sceneSubmissionTaken ||
            source->frame.sceneEncoded)
            return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        if (plane.has_value() != source->worldCanvas.has_value() || (plane && !plane->IsValid()))
            return Result<void>::Failure(MakeError(UiErrors::CanvasSpaceInvalid));
        source->frame.encodedPlane = plane;
        source->frame.sceneEncoded = true;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::DetachHeadsetCanvas */
    Result<UiPresentationReceipt> RuntimeUiPaintPort::DetachHeadsetCanvas(const RuntimeUiInstanceId instance, std::uint64_t frameNumber,
                                                                          const UiWorldCanvasProjection &plane) const {
        using Outcome = Result<UiPresentationReceipt>;
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source || !source->worldCanvas || source->frame.extractedFrame != frameNumber || !source->frame.sceneSubmissionTaken ||
            source->frame.sceneEncoded)
            return Outcome::Failure(MakeError(UiErrors::RevisionStale));
        if (!plane.IsValid())
            return Outcome::Failure(MakeError(UiErrors::CanvasSpaceInvalid));
        const auto &snapshot = source->frame.snapshot->Descriptor();
        const UiPresentationReceipt receipt{snapshot.view,
                                            snapshot.canvas,
                                            snapshot.interactionRevision,
                                            snapshot.snapshotRevision,
                                            UiPresentationOutcome::Presented,
                                            UiPresentationReason::None,
                                            plane};
        source->frame.extractedFrame.reset();
        return Outcome::Success(receipt);
    }

    /** @copydoc RuntimeUiPaintPort::HeadsetCanvasPresented */
    Result<void> RuntimeUiPaintPort::HeadsetCanvasPresented(const RuntimeUiInstanceId instance, UiPresentationReceipt receipt,
                                                            const bool presented) const {
        auto *source = state_ ? state_->ActiveSource(instance) : nullptr;
        if (!source || !source->worldCanvas)
            return Result<void>::Success();
        receipt.outcome = presented ? UiPresentationOutcome::Presented : UiPresentationOutcome::Failed;
        receipt.reason = presented ? UiPresentationReason::None : UiPresentationReason::ExecutionFailure;
        if (!presented)
            receipt.worldProjection.reset();
        if (const auto applied = source->animation->ApplyPresentation(receipt); applied.HasError()) {
            if (const auto code = applied.ErrorValue().code.Value();
                code == UiErrors::RevisionStale.code.Value() || code == UiErrors::RenderPresentationStale.code.Value())
                return Result<void>::Success();
            return Result<void>::Failure(applied.ErrorValue());
        }
        source->output.awaitingPresentation = !presented;
        return Result<void>::Success();
    }

    /** @copydoc Detail::RuntimeUiPaintSource::FinishSceneCanvas */
    Result<void> Detail::RuntimeUiPaintSource::FinishSceneCanvas(const std::uint64_t frameNumber, const bool nativePresentSucceeded) {
        if (frame.extractedFrame != frameNumber)
            return Result<void>::Success();
        const auto &descriptor = frame.snapshot->Descriptor();
        UiPresentationReceipt receipt{descriptor.view, descriptor.canvas, descriptor.interactionRevision, descriptor.snapshotRevision};
        if (frame.sceneEncoded) {
            receipt.outcome = nativePresentSucceeded ? UiPresentationOutcome::Presented : UiPresentationOutcome::Failed;
            receipt.reason = nativePresentSucceeded ? UiPresentationReason::None : UiPresentationReason::ExecutionFailure;
            if (nativePresentSucceeded)
                receipt.worldProjection = frame.encodedPlane;
        }
        if (const auto applied = animation->ApplyPresentation(receipt); applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        output.awaitingPresentation = receipt.outcome != UiPresentationOutcome::Presented;
        frame.extractedFrame.reset();
        frame.encodedPlane.reset();
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::ComposeUi */
    Result<void> RuntimeUiPaintPort::ComposeUi(Render::RenderFrameScope &frame, const std::uint64_t frameNumber) const {
        if (!state_ || !state_->Available())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto sources = state_->Sources();
        std::array<Render::UiPaintProgramSubmission, Detail::MaximumSceneUiSources> batch;
        std::size_t count = 0;
        for (const auto *source : sources) {
            if (!source || source->sceneCanvas)
                continue;
            if (count == batch.size())
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            if (source->output.pending || !source->frame.prepared || !source->programs.active || !source->frame.geometry)
                return Result<void>::Success();
            ++count;
        }
        if (count == 0)
            return Result<void>::Success();
        count = 0;
        for (auto *source : sources)
            if (source && !source->sceneCanvas)
                batch[count++] = {source->programs.active.get(), &*source->frame.prepared, std::move(source->frame.submission)};
        if (const auto submitted = Render::UiPaintProgram::SubmitBatch(frame, {batch.data(), count}); submitted.HasError())
            return submitted;
        for (auto *source : sources) {
            if (!source || source->sceneCanvas)
                continue;
            const auto &descriptor = source->frame.geometry->SourceSnapshot().Descriptor();
            source->frame.encoded = UiPresentationReceipt{descriptor.view,
                                                          descriptor.canvas,
                                                          descriptor.interactionRevision,
                                                          descriptor.snapshotRevision,
                                                          UiPresentationOutcome::Skipped,
                                                          UiPresentationReason::Suppressed};
            source->frame.encodedFrame = frameNumber;
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::Presented */
    Result<void> RuntimeUiPaintPort::Presented(const std::uint64_t frameNumber, const bool nativePresentSucceeded) const {
        if (!state_ || !state_->Available())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto sources = state_->Sources();
        for (const auto *source : sources)
            if (source && source->frame.encoded && source->frame.encodedFrame != frameNumber)
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        for (auto *source : sources) {
            if (!source)
                continue;
            if (source->sceneCanvas) {
                if (const auto completed = source->FinishSceneCanvas(frameNumber, nativePresentSucceeded); completed.HasError())
                    return completed;
                continue;
            }
            if (!source->frame.encoded)
                continue;
            auto receipt = *source->frame.encoded;
            receipt.outcome = nativePresentSucceeded ? UiPresentationOutcome::Presented : UiPresentationOutcome::Failed;
            receipt.reason = nativePresentSucceeded ? UiPresentationReason::None : UiPresentationReason::ExecutionFailure;
            if (const auto applied = source->animation->ApplyPresentation(receipt); applied.HasError())
                return Result<void>::Failure(applied.ErrorValue());
            if (nativePresentSucceeded)
                source->output.awaitingPresentation = false;
            source->frame.encoded.reset();
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
