#include "host/XRSessionHostControl.h"

#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRSessionErrors.h"

namespace Horo::XR::RuntimeInternal {
    /** @copydoc XRSessionHostControl::Retire */
    Result<void> XRSessionHostControl::Retire(XRSessionLifecycle &sessions, XRFrameLifecycle &frames,
                                              const IXRSessionRetirementResources &resources, const XRSessionId &session) {
        if (!session.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (sessions.resources_ != &resources || frames.sessions_ != &sessions)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (sessions.destroyed_ || frames.shutdown_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (sessions.ownedSession_ != session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto state = sessions.snapshot_.state;
        if (state == XRSessionState::Running || state == XRSessionState::Visible || state == XRSessionState::Focused ||
            frames.current_.phase != XRFramePhase::Idle)
            return Result<void>::Failure(MakeError(XRSessionErrors::TransitionInvalid));
        const auto completed = resources.RetirementComplete(session);
        if (completed.HasError())
            return Result<void>::Failure(completed.ErrorValue());
        if (!completed.Value())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));

        // Physical cleanup has already succeeded. These callbacks only retire exact-owner bookkeeping.
        sessions.Retire(session);
        sessions.ownedSession_ = {};
        sessions.snapshot_ = {};
        sessions.acceptedPlan_.reset();
        frames.ResetAfterQuiescence();
        return Result<void>::Success();
    }
}  // namespace Horo::XR::RuntimeInternal
