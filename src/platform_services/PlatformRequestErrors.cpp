#include "Horo/PlatformServices/PlatformRequestErrors.h"

namespace Horo::PlatformServices::RequestErrors {
    namespace {
        const ErrorDomainId Domain{"horo.platform.request"};
    }

    const ErrorCodeDescriptor InvalidConfiguration{.domain = Domain,
                                                   .code = ErrorCode{"platform.request.invalid_configuration"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Platform request configuration is invalid.",
                                                   .remediationHint =
                                                       "Provide finite nonzero capacities, generation, and service deadlines.",
                                                   .retryable = false,
                                                   .userActionable = false};
    const ErrorCodeDescriptor CapacityExceeded{.domain = Domain,
                                               .code = ErrorCode{"platform.request.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Platform request capacity is exhausted.",
                                               .remediationHint = "Retry after active work or retained observers retire.",
                                               .retryable = true,
                                               .userActionable = false};
    const ErrorCodeDescriptor Stale{.domain = Domain,
                                    .code = ErrorCode{"platform.request.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "Platform request identity is stale.",
                                    .remediationHint = "Use a handle issued by the active frontend generation.",
                                    .retryable = false,
                                    .userActionable = false};
    const ErrorCodeDescriptor Expired{.domain = Domain,
                                      .code = ErrorCode{"platform.request.expired"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "Platform request observation has expired.",
                                      .remediationHint = "Observe terminal results within the configured retention bound.",
                                      .retryable = false,
                                      .userActionable = false};
    const ErrorCodeDescriptor InvalidTransition{.domain = Domain,
                                                .code = ErrorCode{"platform.request.invalid_transition"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Platform request state transition is invalid.",
                                                .remediationHint = "Follow the exhaustive platform request lifecycle.",
                                                .retryable = false,
                                                .userActionable = false};
    const ErrorCodeDescriptor FrontendUnavailable{.domain = Domain,
                                                  .code = ErrorCode{"platform.frontend.unavailable"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Platform services frontend is unavailable.",
                                                  .remediationHint = "Submit only while the selected frontend generation is active.",
                                                  .retryable = false,
                                                  .userActionable = false};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"platform.request.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "Platform request was cancelled.",
                                        .remediationHint = "Retry only if the semantic owner still wants the operation.",
                                        .retryable = true,
                                        .userActionable = false};
    const ErrorCodeDescriptor TimedOut{.domain = Domain,
                                       .code = ErrorCode{"platform.request.timed_out"},
                                       .defaultSeverity = ErrorSeverity::Error,
                                       .summary = "Platform request timed out.",
                                       .remediationHint = "Retry only when the operation's idempotency policy permits it.",
                                       .retryable = true,
                                       .userActionable = false};
}  // namespace Horo::PlatformServices::RequestErrors
