#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::RenderSurfaceLifecycleErrors {
    namespace {
        const ErrorDomainId Domain{"render.surface-lifecycle"};
    }  // namespace

    const ErrorCodeDescriptor GenerationExhausted =
        Detail::MakeErrorDescriptor(Domain, "render.surface.generation_exhausted", ErrorSeverity::Critical,
                                    "The surface generation identity space is exhausted.",
                                    "Stop acquisition and create a new process-local surface owner identity.");
    const ErrorCodeDescriptor InvalidCommand =
        Detail::MakeErrorDescriptor(Domain, "render.surface.command_invalid", ErrorSeverity::Error,
                                    "The surface command has an invalid sequence, target shape, or unsupported value.",
                                    "Publish a complete typed command with a non-zero owner sequence.");
    const ErrorCodeDescriptor InvalidIdentity =
        Detail::MakeErrorDescriptor(Domain, "render.surface.identity_invalid", ErrorSeverity::Error, "The surface owner identity is zero.",
                                    "Allocate a non-zero machine-local owner identity before admission.");
    const ErrorCodeDescriptor InvalidOutcome =
        Detail::MakeErrorDescriptor(Domain, "render.surface.outcome_invalid", ErrorSeverity::Error,
                                    "The native realization outcome is incompatible with the frozen command.",
                                    "Report the exact ready, suspended, lost, preserved, or unattached native result.");
    const ErrorCodeDescriptor InvalidState =
        Detail::MakeErrorDescriptor(Domain, "render.surface.state_invalid", ErrorSeverity::Error,
                                    "The requested surface command is invalid in the current lifecycle state.",
                                    "Observe the current snapshot and queue a command allowed by that state.");
    const ErrorCodeDescriptor NoPendingRequest =
        Detail::MakeErrorDescriptor(Domain, "render.surface.request_missing", ErrorSeverity::Info,
                                    "No surface request is waiting at the frame boundary.",
                                    "Continue without native surface work until a platform request is queued.", true);
    const ErrorCodeDescriptor PendingRequestInvalidated =
        Detail::MakeErrorDescriptor(Domain, "render.surface.pending_request_invalidated", ErrorSeverity::Warning,
                                    "The queued surface request is no longer valid after the prior native realization.",
                                    "Observe the current surface state and publish a new owner-sequenced request.", true);
    const ErrorCodeDescriptor RevisionExhausted =
        Detail::MakeErrorDescriptor(Domain, "render.surface.revision_exhausted", ErrorSeverity::Critical,
                                    "The surface publication revision space is exhausted.",
                                    "Stop acquisition and create a new process-local surface owner identity.");
    const ErrorCodeDescriptor StaleRequest =
        Detail::MakeErrorDescriptor(Domain, "render.surface.request_stale", ErrorSeverity::Warning,
                                    "The surface request sequence did not advance.",
                                    "Discard the stale platform event and publish a newer owner sequence.", true);
    const ErrorCodeDescriptor StaleTransition =
        Detail::MakeErrorDescriptor(Domain, "render.surface.transition_stale", ErrorSeverity::Warning,
                                    "The completion does not match the exact frozen surface transition.",
                                    "Discard the late result and complete only the current owner, generation, revision, and sequence.",
                                    true);
    const ErrorCodeDescriptor TransitionBusy =
        Detail::MakeErrorDescriptor(Domain, "render.surface.transition_busy", ErrorSeverity::Error,
                                    "A native surface transition is already in flight.",
                                    "Complete the frozen transition before beginning the next safe-point candidate.");
    const ErrorCodeDescriptor WrongThread =
        Detail::MakeErrorDescriptor(Domain, "render.surface.thread_affinity_violation", ErrorSeverity::Error,
                                    "Surface lifecycle mutation was attempted from a non-owner thread.",
                                    "Transfer the typed request or result to the renderer owner-thread safe point.");
}  // namespace Horo::Render::RenderSurfaceLifecycleErrors
