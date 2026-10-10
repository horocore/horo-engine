#include "Horo/Runtime/Render/FramePacingErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::FramePacingErrors {
    namespace {
        const ErrorDomainId Domain{"render.frame-pacing"};
    }

    const ErrorCodeDescriptor InvalidPolicy =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.policy_invalid", ErrorSeverity::Error, "Frame cap exceeds 1000 Hz.",
                                    "Use zero for unlimited or a cap from 1 to 1000 Hz.");
    const ErrorCodeDescriptor InvalidSurface =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.surface_invalid", ErrorSeverity::Error,
                                    "Pacing requires a valid committed surface publication.",
                                    "Publish the renderer-owned lifecycle snapshot before pacing.");
    const ErrorCodeDescriptor StaleEvidence =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.evidence_stale", ErrorSeverity::Error,
                                    "Surface revision or presentation evidence is stale.",
                                    "Discard old-generation evidence and publish increasing revisions and frame ordinals.");
    const ErrorCodeDescriptor InvalidClock =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.clock_invalid", ErrorSeverity::Error,
                                    "Presentation clock is negative, reversed or outside the safe deadline range.",
                                    "Use one qualified monotonic host clock and reset baselines after discontinuity.");
    const ErrorCodeDescriptor InvalidNativeTiming =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.native_timing_invalid", ErrorSeverity::Error,
                                    "Native display evidence is inconsistent or outside bounded interval limits.",
                                    "Translate exact display timestamps and refresh ordinals in the host clock domain.");
    const ErrorCodeDescriptor NativeTimingUnsupported =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.native_timing_unsupported", ErrorSeverity::Error,
                                    "This backend has no qualified native display timing provider.",
                                    "Observe CPU present-call duration separately; do not infer scanout or refresh.");
    const ErrorCodeDescriptor WrongThread = Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.wrong_thread", ErrorSeverity::Error,
                                                                        "Frame pacing operation is outside the host owner thread.",
                                                                        "Marshal pacing and diagnostics onto the host owner thread.");
    const ErrorCodeDescriptor Cancelled =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.cancelled", ErrorSeverity::Error, "Host pacing was cancelled.",
                                    "Stop scheduling frames and shut down the host-owned surface.");
    const ErrorCodeDescriptor Stopped =
        Detail::MakeErrorDescriptor(Domain, "render.frame_pacing.stopped", ErrorSeverity::Error, "Host pacing admission is closed.",
                                    "Create a new pacing owner for a restarted host.");
}  // namespace Horo::Render::FramePacingErrors
