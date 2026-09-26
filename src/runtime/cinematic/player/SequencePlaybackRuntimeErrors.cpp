#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"

namespace Horo::Cinematic::SequencePlaybackRuntimeErrors {
    namespace {
        const ErrorDomainId PlaybackDomain{"horo.cinematic.playback_runtime"};
    }

    const ErrorCodeDescriptor SessionInvalid{.domain = PlaybackDomain,
                                             .code = ErrorCode{"cinematic.playback_runtime.session_invalid"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The cinematic runtime session identity is invalid.",
                                             .remediationHint = "Create the playback service with a host-issued session generation."};
    const ErrorCodeDescriptor BudgetInvalid{.domain = PlaybackDomain,
                                            .code = ErrorCode{"cinematic.playback_runtime.budget_invalid"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The cinematic evaluation budget is not canonical.",
                                            .remediationHint = "Use one of the validated Compact, Standard, or Large profiles."};
    const ErrorCodeDescriptor AdmissionClosed{.domain = PlaybackDomain,
                                              .code = ErrorCode{"cinematic.playback_runtime.admission_closed"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The cinematic runtime no longer admits new playback.",
                                              .remediationHint = "Retire the current session and create a new runtime service."};
    const ErrorCodeDescriptor DuplicateHandle{.domain = PlaybackDomain,
                                              .code = ErrorCode{"cinematic.playback_runtime.duplicate_handle"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The cinematic player identity is already active.",
                                              .remediationHint = "Use the existing generation or release the retired player first."};
    const ErrorCodeDescriptor ActivationInvalid{.domain = PlaybackDomain,
                                                .code = ErrorCode{"cinematic.playback_runtime.activation_invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The cinematic playback activation transaction is malformed.",
                                                .remediationHint =
                                                    "Rebuild the plan, authority claims, and handoff snapshot at the owner boundary."};
    const ErrorCodeDescriptor ClockInvalid{.domain = PlaybackDomain,
                                           .code = ErrorCode{"cinematic.playback_runtime.clock_invalid"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The host cinematic clock sample is invalid for this player.",
                                           .remediationHint =
                                               "Supply the selected monotonic source and a valid scale at the owner boundary."};
    const ErrorCodeDescriptor CapacityExceeded{.domain = PlaybackDomain,
                                               .code = ErrorCode{"cinematic.playback_runtime.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Warning,
                                               .summary = "The selected cinematic tier cannot admit this playback load.",
                                               .remediationHint = "Select a larger validated tier or reduce the active sequence load."};
    const ErrorCodeDescriptor AuthorityConflict{.domain = PlaybackDomain,
                                                .code = ErrorCode{"cinematic.playback_runtime.authority_conflict"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Required cinematic authority claims conflict.",
                                                .remediationHint =
                                                    "Declare one compatible owner or assign distinct channels and priorities."};
    const ErrorCodeDescriptor RestoreInvalid{.domain = PlaybackDomain,
                                             .code = ErrorCode{"cinematic.playback_runtime.restore_invalid"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The cinematic restore snapshot is invalid.",
                                             .remediationHint = "Capture unique generation-fenced targets before playback activation."};
    const ErrorCodeDescriptor RestoreRequired{.domain = PlaybackDomain,
                                              .code = ErrorCode{"cinematic.playback_runtime.restore_required"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The terminal player still owns an unapplied restore snapshot.",
                                              .remediationHint = "Apply restore at the owner safe point before releasing the player."};
    const ErrorCodeDescriptor PlayerNotTerminal{.domain = PlaybackDomain,
                                                .code = ErrorCode{"cinematic.playback_runtime.player_not_terminal"},
                                                .defaultSeverity = ErrorSeverity::Warning,
                                                .summary = "The player must be terminal before this lifecycle operation.",
                                                .remediationHint = "Stop or cancel the player and wait for its terminal boundary."};
    const ErrorCodeDescriptor HandleInvalid{.domain = PlaybackDomain,
                                            .code = ErrorCode{"cinematic.playback_runtime.handle_invalid"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The cinematic player handle is invalid.",
                                            .remediationHint = "Use the exact handle returned by the session-owned service."};
    const ErrorCodeDescriptor HandleUnknown{.domain = PlaybackDomain,
                                            .code = ErrorCode{"cinematic.playback_runtime.handle_unknown"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The cinematic player handle is not owned by this service.",
                                            .remediationHint = "Submit the handle to its owning runtime session."};
    const ErrorCodeDescriptor HandleStale{.domain = PlaybackDomain,
                                          .code = ErrorCode{"cinematic.playback_runtime.handle_stale"},
                                          .defaultSeverity = ErrorSeverity::Warning,
                                          .summary = "The cinematic player handle names a retired generation.",
                                          .remediationHint = "Discard the handle and use the newly issued generation."};
    const ErrorCodeDescriptor RevisionExhausted{.domain = PlaybackDomain,
                                                .code = ErrorCode{"cinematic.playback_runtime.revision_exhausted"},
                                                .defaultSeverity = ErrorSeverity::Critical,
                                                .summary = "The cinematic playback revision cannot advance safely.",
                                                .remediationHint = "Retire the player and do not wrap its generation or revision."};
}  // namespace Horo::Cinematic::SequencePlaybackRuntimeErrors
