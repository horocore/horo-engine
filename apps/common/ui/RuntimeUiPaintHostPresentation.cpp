#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>

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
        if (!state_ || !state_->Available())
            return false;
        return std::ranges::any_of(state_->Sources(), [view](const auto *source) {
            return source && !source->output.pending && !source->output.awaitingPresentation && source->animation->InputEligible(view);
        });
    }

    /** @copydoc RuntimeUiPaintPort::ComposeUi */
    Result<void> RuntimeUiPaintPort::ComposeUi(Render::RenderFrameScope &frame, const std::uint64_t frameNumber) const {
        if (!state_ || !state_->Available())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto sources = state_->Sources();
        std::array<Render::UiPaintProgramSubmission, Detail::MaximumSceneUiSources> batch;
        std::size_t count = 0;
        for (const auto *source : sources) {
            if (!source)
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
            if (source)
                batch[count++] = {source->programs.active.get(), &*source->frame.prepared, std::move(source->frame.submission)};
        if (const auto submitted = Render::UiPaintProgram::SubmitBatch(frame, {batch.data(), count}); submitted.HasError())
            return submitted;
        for (auto *source : sources) {
            if (!source)
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
            if (!source || !source->frame.encoded)
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
