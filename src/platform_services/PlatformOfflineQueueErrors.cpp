#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"

#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"

namespace Horo::PlatformServices::OfflineQueueErrors {
    namespace {
        const ErrorDomainId Domain{"horo.platform.offline"};
    }

    const ErrorCodeDescriptor &InvalidConfiguration = ::Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors::InvalidConfiguration;
    const ErrorCodeDescriptor InvalidIntent{.domain = Domain,
                                            .code = ErrorCode{"platform.offline.invalid_intent"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Platform offline intent is malformed or expired.",
                                            .remediationHint = "Submit the original typed intent with a valid lane and bounded payload.",
                                            .retryable = false,
                                            .userActionable = false};
    const ErrorCodeDescriptor &CapacityExceeded = ::Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors::CapacityExceeded;
    const ErrorCodeDescriptor &IdentityConflict = ::Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors::IdentityConflict;
    const ErrorCodeDescriptor Stale{.domain = Domain,
                                    .code = ErrorCode{"platform.offline.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "Platform offline operation handle is stale.",
                                    .remediationHint = "Use a handle issued by the active offline queue generation.",
                                    .retryable = false,
                                    .userActionable = false};
    const ErrorCodeDescriptor Expired{.domain = Domain,
                                      .code = ErrorCode{"platform.offline.expired"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "Platform offline intent expired or its observation was compacted.",
                                      .remediationHint = "Observe the typed terminal outcome before the retention horizon ends.",
                                      .retryable = false,
                                      .userActionable = false};
    const ErrorCodeDescriptor InvalidTransition{.domain = Domain,
                                                .code = ErrorCode{"platform.offline.invalid_transition"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Platform offline queue state transition is invalid.",
                                                .remediationHint = "Follow the queue-owned ordering and ambiguity lifecycle.",
                                                .retryable = false,
                                                .userActionable = false};
    const ErrorCodeDescriptor ClockMovedBackward{.domain = Domain,
                                                 .code = ErrorCode{"platform.offline.clock_moved_backward"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Platform offline queue monotonic time moved backward.",
                                                 .remediationHint = "Use the owner's monotonic clock and preserve its high-water mark.",
                                                 .retryable = false,
                                                 .userActionable = false};
    const ErrorCodeDescriptor QueueUnavailable{.domain = Domain,
                                               .code = ErrorCode{"platform.offline.unavailable"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Platform offline queue admission, dispatch, or resumption is closed.",
                                               .remediationHint = "Reopen a queue only from a newly validated owner generation.",
                                               .retryable = false,
                                               .userActionable = false};
}  // namespace Horo::PlatformServices::OfflineQueueErrors
