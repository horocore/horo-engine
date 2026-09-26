#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

namespace Horo::PlatformServices::PlatformProgressionErrors {
    namespace {
        const ErrorDomainId Domain{"horo.platform.progression"};
    }

    const ErrorCodeDescriptor InvalidConfiguration{.domain = Domain,
                                                   .code = ErrorCode{"platform.progression.invalid_configuration"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Progression idempotency configuration is invalid.",
                                                   .remediationHint = "Provide a finite nonzero in-flight capacity.",
                                                   .retryable = false,
                                                   .userActionable = false};
    const ErrorCodeDescriptor InvalidMutation{.domain = Domain,
                                              .code = ErrorCode{"platform.progression.invalid_mutation"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Progression mutation identity or envelope is invalid.",
                                              .remediationHint = "Submit a complete typed mutation from the active authority scope.",
                                              .retryable = false,
                                              .userActionable = false};
    const ErrorCodeDescriptor IdempotencyConflict{.domain = Domain,
                                                  .code = ErrorCode{"platform.progression.idempotency_conflict"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "A progression mutation ID was reused with another envelope.",
                                                  .remediationHint = "Retain the original mutation envelope; last writer never wins.",
                                                  .retryable = false,
                                                  .userActionable = false};
    const ErrorCodeDescriptor CapacityExceeded{.domain = Domain,
                                               .code = ErrorCode{"platform.progression.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Progression in-flight mutation capacity is exhausted.",
                                               .remediationHint = "Retire terminal work before admitting another mutation.",
                                               .retryable = true,
                                               .userActionable = false};
    const ErrorCodeDescriptor Closed{.domain = Domain,
                                     .code = ErrorCode{"platform.progression.closed"},
                                     .defaultSeverity = ErrorSeverity::Error,
                                     .summary = "Progression mutation admission is closed.",
                                     .remediationHint = "Submit only while the owning platform-services generation is active.",
                                     .retryable = false,
                                     .userActionable = false};
}  // namespace Horo::PlatformServices::PlatformProgressionErrors
