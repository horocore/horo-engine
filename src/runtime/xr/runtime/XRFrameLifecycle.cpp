#include "Horo/XR/XRFrameLifecycle.h"

namespace Horo::XR {
    /** @copydoc XRFrameLifecycle::XRFrameLifecycle */
    XRFrameLifecycle::XRFrameLifecycle(XRSessionLifecycle &sessions) noexcept : sessions_(&sessions) {}

    /** @copydoc XRFrameLifecycle::BindConfiguration */
    XRFrameStatus XRFrameLifecycle::BindConfiguration(const XRViewConfigurationId &configuration, const XRCapabilitySnapshot &capabilities,
                                                      const XRFrameLimits limits) noexcept {
        if (shutdown_)
            return XRFrameStatus::Shutdown;
        if (current_.phase != XRFramePhase::Idle)
            return XRFrameStatus::OutOfOrder;
        if (!configuration.IsValid())
            return XRFrameStatus::InvalidInput;
        const XRSessionSnapshot session = sessions_->Snapshot();
        if (configuration.session != session.session || capabilities.System() != session.session.system)
            return XRFrameStatus::StaleSession;
        if (capabilities.Revision() != session.capabilityRevision)
            return XRFrameStatus::StalePlan;
        if (limits.maximumViews > capabilities.Limits().maximumViews || limits.maximumImages > XRFrameHardLimits::MaximumImages ||
            limits.maximumLayers > XRFrameHardLimits::MaximumLayers)
            return XRFrameStatus::CapacityExceeded;

        configuration_ = configuration;
        revision_ = capabilities.Revision();
        limits_ = limits;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::ReserveWait */
    XRFrameWaitOutcome XRFrameLifecycle::ReserveWait(const XRSessionId &session) noexcept {
        if (shutdown_)
            return {XRFrameStatus::Shutdown, {}};
        if (!session.IsValid())
            return {XRFrameStatus::InvalidInput, {}};
        if (!configuration_.IsValid())
            return {XRFrameStatus::Unavailable, {}};
        const XRSessionSnapshot active = sessions_->Snapshot();
        if (session != active.session || configuration_.session != session)
            return {XRFrameStatus::StaleSession, {}};
        if (revision_ != active.capabilityRevision)
            return {XRFrameStatus::StalePlan, {}};
        if (!active.admitsFrames)
            return {XRFrameStatus::Unavailable, {}};
        if (current_.phase != XRFramePhase::Idle)
            return {XRFrameStatus::Duplicate, {}};
        if (nextSequence_ == 0)
            return {XRFrameStatus::SequenceExhausted, {}};

        const XRFrameId frame{session, configuration_, revision_, nextSequence_};
        ++nextSequence_;  // Zero after the maximum sequence permanently prevents reuse.
        current_.frame = frame;
        current_.phase = XRFramePhase::WaitReserved;
        return {XRFrameStatus::Ok, frame};
    }

    /** @copydoc XRFrameLifecycle::CancelWait */
    XRFrameStatus XRFrameLifecycle::CancelWait() noexcept {
        if (shutdown_)
            return XRFrameStatus::Shutdown;
        if (current_.phase != XRFramePhase::WaitReserved)
            return XRFrameStatus::OutOfOrder;
        ClearFrame();
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::RecordWait */
    XRFrameWaitOutcome XRFrameLifecycle::RecordWait(const XRRenderPredictionTime predictedDisplayTime, const bool shouldRender) noexcept {
        if (shutdown_)
            return {XRFrameStatus::Shutdown, {}};
        if (current_.phase != XRFramePhase::WaitReserved)
            return {XRFrameStatus::OutOfOrder, {}};

        const XRFrameId frame = current_.frame;
        current_.predictedDisplayTime = predictedDisplayTime;
        current_.phase = XRFramePhase::Waited;
        current_.shouldRender = shouldRender;
        if (!predictedDisplayTime.IsValid())
            return {XRFrameStatus::InvalidInput, frame};
        if (shouldRender && (limits_.maximumViews == 0 || limits_.maximumImages == 0 || limits_.maximumLayers == 0))
            return {XRFrameStatus::Unsupported, frame};
        current_.renderAdmitted = shouldRender;
        return {XRFrameStatus::Ok, frame};
    }

    /** @copydoc XRFrameLifecycle::Begin */
    XRFrameStatus XRFrameLifecycle::Begin(const XRFrameId &frame) noexcept {
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (current_.phase != XRFramePhase::Waited)
            return XRFrameStatus::OutOfOrder;
        current_.phase = XRFramePhase::Begun;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::LocateViews */
    XRFrameStatus XRFrameLifecycle::LocateViews(const XRFrameId &frame, const std::uint32_t viewCount) noexcept {
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (!current_.renderAdmitted)
            return XRFrameStatus::Unsupported;
        if (current_.phase != XRFramePhase::Begun)
            return XRFrameStatus::OutOfOrder;
        if (viewCount == 0)
            return XRFrameStatus::InvalidInput;
        if (viewCount > limits_.maximumViews)
            return XRFrameStatus::CapacityExceeded;
        current_.locatedViews = viewCount;
        current_.phase = XRFramePhase::ViewsLocated;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::Acquire */
    XRFrameStatus XRFrameLifecycle::Acquire(const XRFrameId &frame, const XRSwapchainImageId &image) noexcept {
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (!current_.renderAdmitted)
            return XRFrameStatus::Unsupported;
        if (current_.phase != XRFramePhase::ViewsLocated && current_.phase != XRFramePhase::ImagesAcquired)
            return XRFrameStatus::OutOfOrder;
        if (!image.IsValid())
            return XRFrameStatus::InvalidInput;
        if (image.target.session != frame.session)
            return XRFrameStatus::StaleSession;
        if (FindImage(image) != XRFrameHardLimits::MaximumImages)
            return XRFrameStatus::Duplicate;
        if (current_.acquiredImages >= limits_.maximumImages)
            return XRFrameStatus::CapacityExceeded;

        images_[current_.acquiredImages] = image;
        released_[current_.acquiredImages] = false;
        ++current_.acquiredImages;
        current_.phase = XRFramePhase::ImagesAcquired;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::Submit */
    XRFrameStatus XRFrameLifecycle::Submit(const XRFrameId &frame, const std::span<const XRSwapchainImageId> completedImages) noexcept {
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (!current_.renderAdmitted)
            return XRFrameStatus::Unsupported;
        if (current_.phase != XRFramePhase::ImagesAcquired)
            return XRFrameStatus::OutOfOrder;
        if (completedImages.size() != current_.acquiredImages)
            return XRFrameStatus::IncompleteSubmission;
        for (std::uint32_t index = 0; index < current_.acquiredImages; ++index) {
            if (!completedImages[index].IsValid())
                return XRFrameStatus::InvalidInput;
            if (completedImages[index].target.session != frame.session)
                return XRFrameStatus::StaleSession;
            if (completedImages[index] != images_[index])
                return XRFrameStatus::IncompleteSubmission;
        }
        submitted_ = true;
        current_.phase = XRFramePhase::RendererSubmitted;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::Abort */
    XRFrameStatus XRFrameLifecycle::Abort(const XRFrameId &frame) noexcept {
        using enum XRFramePhase;
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (!current_.renderAdmitted)
            return XRFrameStatus::Unsupported;
        if (current_.phase != Begun && current_.phase != ViewsLocated && current_.phase != ImagesAcquired &&
            current_.phase != RendererSubmitted && current_.phase != ImagesReleased)
            return XRFrameStatus::OutOfOrder;
        aborted_ = true;
        current_.phase = Aborting;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::Release */
    XRFrameStatus XRFrameLifecycle::Release(const XRFrameId &frame, const XRSwapchainImageId &image) noexcept {
        using enum XRFramePhase;
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (current_.phase != RendererSubmitted && current_.phase != Aborting && current_.phase != ImagesReleased)
            return XRFrameStatus::OutOfOrder;
        if (!image.IsValid())
            return XRFrameStatus::InvalidInput;
        if (image.target.session != frame.session)
            return XRFrameStatus::StaleSession;
        const std::uint32_t index = FindImage(image);
        if (index == XRFrameHardLimits::MaximumImages)
            return XRFrameStatus::ImageNotAcquired;
        if (released_[index])
            return XRFrameStatus::Duplicate;
        released_[index] = true;
        ++current_.releasedImages;
        if (current_.releasedImages == current_.acquiredImages)
            current_.phase = ImagesReleased;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::End */
    XRFrameStatus XRFrameLifecycle::End(const XRFrameId &frame, const std::uint32_t layerCount) noexcept {
        if (const XRFrameStatus valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (current_.phase == XRFramePhase::Waited)
            return XRFrameStatus::OutOfOrder;
        if (layerCount > limits_.maximumLayers)
            return XRFrameStatus::CapacityExceeded;
        if (current_.releasedImages != current_.acquiredImages)
            return XRFrameStatus::ImageNotReleased;
        if (current_.renderAdmitted) {
            if (layerCount == 0) {
                if (!aborted_)
                    return XRFrameStatus::OutOfOrder;
            } else if (aborted_ || !submitted_ || current_.phase != XRFramePhase::ImagesReleased) {
                return XRFrameStatus::OutOfOrder;
            }
        } else if (layerCount != 0 || current_.phase != XRFramePhase::Begun) {
            return XRFrameStatus::OutOfOrder;
        }

        lastEnded_ = frame;
        ClearFrame();
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::Snapshot */
    XRFrameSnapshot XRFrameLifecycle::Snapshot() const noexcept {
        return current_;
    }

    /** @copydoc XRFrameLifecycle::ResetAfterQuiescence */
    void XRFrameLifecycle::ResetAfterQuiescence() noexcept {
        ClearFrame();
        configuration_ = {};
        revision_ = {};
        limits_ = {};
    }

    /** @copydoc XRFrameLifecycle::Shutdown */
    void XRFrameLifecycle::Shutdown() noexcept {
        if (shutdown_)
            return;
        ResetAfterQuiescence();
        shutdown_ = true;
    }

    /** @copydoc XRFrameLifecycle::ValidateCurrent */
    XRFrameStatus XRFrameLifecycle::ValidateCurrent(const XRFrameId &frame) const noexcept {
        if (shutdown_)
            return XRFrameStatus::Shutdown;
        if (!frame.IsValid())
            return XRFrameStatus::InvalidInput;
        const XRSessionSnapshot session = sessions_->Snapshot();
        if (frame.session != session.session)
            return XRFrameStatus::StaleSession;
        if (frame.capabilityRevision != session.capabilityRevision || frame.configuration != configuration_ ||
            frame.capabilityRevision != revision_)
            return XRFrameStatus::StalePlan;
        if (current_.phase == XRFramePhase::Idle)
            return frame == lastEnded_ ? XRFrameStatus::Duplicate : XRFrameStatus::StaleFrame;
        return frame == current_.frame ? XRFrameStatus::Ok : XRFrameStatus::StaleFrame;
    }

    /** @copydoc XRFrameLifecycle::FindImage */
    std::uint32_t XRFrameLifecycle::FindImage(const XRSwapchainImageId &image) const noexcept {
        for (std::uint32_t index = 0; index < current_.acquiredImages; ++index) {
            if (images_[index] == image)
                return index;
        }
        return XRFrameHardLimits::MaximumImages;
    }

    /** @copydoc XRFrameLifecycle::ClearFrame */
    void XRFrameLifecycle::ClearFrame() noexcept {
        current_ = {};
        images_.fill({});
        released_.fill(false);
        aborted_ = false;
        submitted_ = false;
    }
}  // namespace Horo::XR
