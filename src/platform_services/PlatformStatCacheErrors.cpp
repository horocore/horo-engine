#include "Horo/PlatformServices/PlatformStatCacheCoordinator.h"

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.stat"};
    }  // namespace

    namespace StatCoordinatorErrors {
        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.stat.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "The stat cache coordinator configuration is invalid.",
                                                       "Use finite nonzero cache, queue, ledger, and freshness limits.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"platform.stat.invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "The stat request is malformed.",
                                                 "Provide a valid typed subject, stat, value, and mutation envelope.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor UnknownStat{Domain,
                                              ErrorCode{"platform.stat.unknown"},
                                              ErrorSeverity::Error,
                                              "The stat is not registered in the immutable project registry.",
                                              "Resolve an active authored stat identity before admission.",
                                              false,
                                              false};
        const ErrorCodeDescriptor AuthorityDenied{Domain,
                                                  ErrorCode{"platform.stat.authority_denied"},
                                                  ErrorSeverity::Error,
                                                  "The stat write authority is not permitted.",
                                                  "Submit through the authority selected by the registered definition.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor InvalidValue{Domain,
                                               ErrorCode{"platform.stat.value_invalid"},
                                               ErrorSeverity::Error,
                                               "The stat value contradicts its registered numeric schema.",
                                               "Use the registered representation and inclusive value range.",
                                               false,
                                               false};
        const ErrorCodeDescriptor RevisionRequired{Domain,
                                                   ErrorCode{"platform.stat.revision_required"},
                                                   ErrorSeverity::Error,
                                                   "The stat operation requires an exact provider revision.",
                                                   "Supply a nonzero revision captured from the authoritative query.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor IdempotencyConflict{Domain,
                                                      ErrorCode{"platform.stat.idempotency_conflict"},
                                                      ErrorSeverity::Error,
                                                      "A stat mutation ID was reused with different content.",
                                                      "Allocate a new logical mutation identity for different content.",
                                                      false,
                                                      false};
        const ErrorCodeDescriptor CapacityExceeded{Domain,
                                                   ErrorCode{"platform.stat.capacity_exceeded"},
                                                   ErrorSeverity::Error,
                                                   "The bounded stat coordinator cannot retain more work or cache state.",
                                                   "Drain provider work or apply the product capacity policy.",
                                                   true,
                                                   false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.stat.closed"},
                                         ErrorSeverity::Error,
                                         "The stat cache coordinator is closed.",
                                         "Compose a new coordinator for the current session.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StalePublication{Domain,
                                                   ErrorCode{"platform.stat.stale_publication"},
                                                   ErrorSeverity::Error,
                                                   "The stat publication belongs to an obsolete session or request.",
                                                   "Discard the completion and reconcile through the current session.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor InvalidState{Domain,
                                               ErrorCode{"platform.stat.state_invalid"},
                                               ErrorSeverity::Error,
                                               "The provider returned malformed stat state.",
                                               "Reject the response and inspect the provider adapter contract.",
                                               false,
                                               false};
        const ErrorCodeDescriptor StaleState{Domain,
                                             ErrorCode{"platform.stat.state_stale"},
                                             ErrorSeverity::Error,
                                             "The provider stat state belongs to an obsolete session or access policy.",
                                             "Discard it and query the current session generation.",
                                             false,
                                             false};
        const ErrorCodeDescriptor CacheCorrupt{Domain,
                                               ErrorCode{"platform.stat.cache_corrupt"},
                                               ErrorSeverity::Error,
                                               "A restored stat cache record is corrupt or belongs to another namespace.",
                                               "Quarantine the record and perform an explicit provider read.",
                                               false,
                                               false};
    }  // namespace StatCoordinatorErrors
}  // namespace Horo::PlatformServices
