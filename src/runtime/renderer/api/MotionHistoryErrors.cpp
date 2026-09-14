#include "Horo/Runtime/Render/MotionHistoryErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::MotionHistoryErrors {
    namespace {
        const ErrorDomainId Domain{"render.motion_history"};
    }

    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.allocation_failed", ErrorSeverity::Error,
                                    "Motion-history storage allocation failed.",
                                    "Reduce the admitted object capacity or release host memory before retrying.", true);
    const ErrorCodeDescriptor FrameAlreadyPending =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.frame_pending", ErrorSeverity::Error,
                                    "A motion-history frame is already pending.",
                                    "Publish or abandon the pending frame before beginning another frame.");
    const ErrorCodeDescriptor InvalidFrame =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.frame_invalid", ErrorSeverity::Error,
                                    "The motion-history frame is stale or was modified.",
                                    "Complete the exact frame returned by BeginFrame.");
    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.limits_invalid", ErrorSeverity::Error,
                                    "The motion-history object limit is zero or exceeds the hard bound.",
                                    "Provide a finite non-zero capacity within the documented maximum.");
    const ErrorCodeDescriptor InvalidRequest =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.request_invalid", ErrorSeverity::Error,
                                    "The motion-history frame request is incomplete, duplicated, or over capacity.",
                                    "Provide complete compatibility and semantic inputs with canonical unique object identities.");
    const ErrorCodeDescriptor TrackerStopped =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.tracker_stopped", ErrorSeverity::Error,
                                    "The motion-history tracker has stopped.", "Create a new tracker for a new renderer lifetime.");
    const ErrorCodeDescriptor WrongThread =
        Detail::MakeErrorDescriptor(Domain, "render.motion_history.wrong_thread", ErrorSeverity::Error,
                                    "Motion-history mutation used a non-owner thread.",
                                    "Perform motion-history lifecycle operations at the renderer owner-thread safe point.");
}  // namespace Horo::Render::MotionHistoryErrors
