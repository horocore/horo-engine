#include "RuntimeUiPaintHostInternal.h"

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    /** @copydoc RuntimeUiPaintPort::AcquireFrame */
    Result<UiAnimationFrameLease> RuntimeUiPaintPort::AcquireFrame() const {
        if (!state_ || !state_->Available() || !state_->source || state_->phase != RuntimeUiPaintHostPhase::Active)
            return Result<UiAnimationFrameLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        return state_->source->animation->Acquire();
    }

    /** @copydoc RuntimeUiPaintPort::InputEligible */
    bool RuntimeUiPaintPort::InputEligible(const UiRenderViewId view) const noexcept {
        return state_ && state_->Available() && state_->source && state_->phase == RuntimeUiPaintHostPhase::Active &&
               !state_->source->output.pending && !state_->source->output.awaitingPresentation &&
               state_->source->animation->InputEligible(view);
    }

    /** @copydoc RuntimeUiPaintPort::ComposeUi */
    Result<void> RuntimeUiPaintPort::ComposeUi(Render::RenderFrameScope &frame, const std::uint64_t frameNumber) const {
        if (!state_ || !state_->Available())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!state_->source || state_->phase != RuntimeUiPaintHostPhase::Active)
            return Result<void>::Success();
        auto &source = *state_->source;
        if (source.output.pending || !source.frame.prepared || !source.program || !source.frame.geometry)
            return Result<void>::Success();
        const auto &descriptor = source.frame.geometry->SourceSnapshot().Descriptor();
        const auto submitted = source.program->Submit(frame, *source.frame.prepared, std::move(source.frame.submission));
        if (submitted.HasError())
            return submitted;
        source.frame.encoded = UiPresentationReceipt{descriptor.view,
                                                     descriptor.canvas,
                                                     descriptor.interactionRevision,
                                                     descriptor.snapshotRevision,
                                                     UiPresentationOutcome::Skipped,
                                                     UiPresentationReason::Suppressed};
        source.frame.encodedFrame = frameNumber;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::Presented */
    Result<void> RuntimeUiPaintPort::Presented(const std::uint64_t frameNumber, const bool nativePresentSucceeded) const {
        if (!state_ || !state_->Available())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!state_->source || !state_->source->frame.encoded)
            return Result<void>::Success();
        auto &source = *state_->source;
        if (source.frame.encodedFrame != frameNumber)
            return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
        auto receipt = *source.frame.encoded;
        receipt.outcome = nativePresentSucceeded ? UiPresentationOutcome::Presented : UiPresentationOutcome::Failed;
        receipt.reason = nativePresentSucceeded ? UiPresentationReason::None : UiPresentationReason::ExecutionFailure;
        const auto applied = source.animation->ApplyPresentation(receipt);
        if (applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        if (nativePresentSucceeded)
            source.output.awaitingPresentation = false;
        source.frame.encoded.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
