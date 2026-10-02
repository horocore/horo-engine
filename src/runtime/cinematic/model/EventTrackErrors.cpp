#include "Horo/Cinematic/EventTrackErrors.h"

namespace Horo::Cinematic::EventTrackErrors {
    namespace {
        const ErrorDomainId Domain{"horo.cinematic.event"};
    }

    const ErrorCodeDescriptor UnknownName{.domain = Domain,
                                          .code = ErrorCode{"cinematic.event.unknown_name"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The authored cinematic event name is not declared.",
                                          .remediationHint = "Register the qualified event or repair the keyframe name before cooking.",
                                          .userActionable = true};
    const ErrorCodeDescriptor SchemaMismatch{.domain = Domain,
                                             .code = ErrorCode{"cinematic.event.schema_mismatch"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The cinematic event payload does not match its versioned export descriptor.",
                                             .remediationHint = "Migrate the event payload to the declared function parameter schema.",
                                             .userActionable = true};
    const ErrorCodeDescriptor CookInvalid{.domain = Domain,
                                          .code = ErrorCode{"cinematic.event.cook_invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The cinematic event cook input has an invalid or duplicate identity.",
                                          .remediationHint = "Repair the colliding event descriptor or keyframe identity.",
                                          .userActionable = true};
    const ErrorCodeDescriptor CookCapacityExceeded{.domain = Domain,
                                                   .code = ErrorCode{"cinematic.event.cook_capacity_exceeded"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The cinematic event cook input exceeds a finite bound.",
                                                   .remediationHint = "Reduce event key or payload count before cooking.",
                                                   .userActionable = true};
    const ErrorCodeDescriptor BindingUnavailable{.domain = Domain,
                                                 .code = ErrorCode{"cinematic.event.binding_unavailable"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "A required cooked cinematic event handler is unavailable.",
                                                 .remediationHint =
                                                     "Activate the matching module and capability before starting the player."};
    const ErrorCodeDescriptor StaleBinding{.domain = Domain,
                                           .code = ErrorCode{"cinematic.event.stale_binding"},
                                           .defaultSeverity = ErrorSeverity::Warning,
                                           .summary = "A cinematic event handler or player generation was retired.",
                                           .remediationHint = "Discard the stale occurrence and activate a new player generation."};
    const ErrorCodeDescriptor DispatchCapacityExceeded{.domain = Domain,
                                                       .code = ErrorCode{"cinematic.event.dispatch_capacity_exceeded"},
                                                       .defaultSeverity = ErrorSeverity::Warning,
                                                       .summary = "The cinematic event queue cannot reserve the complete batch.",
                                                       .remediationHint = "Hold the player and retry a later admitted boundary."};
    const ErrorCodeDescriptor DispatchStateInvalid{.domain = Domain,
                                                   .code = ErrorCode{"cinematic.event.dispatch_state_invalid"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The cinematic event dispatcher is in an invalid lifecycle state.",
                                                   .remediationHint =
                                                       "Use the session owner safe point and close admission before teardown."};
}  // namespace Horo::Cinematic::EventTrackErrors
