#include "Horo/Release/UpdateRetentionErrors.h"

namespace Horo::Release::UpdateRetentionErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update.retention"};
    }

    const ErrorCodeDescriptor InvalidSnapshot{.domain = Domain,
                                              .code = ErrorCode{"invalid_snapshot"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Installed-version retention snapshot is invalid."};
    const ErrorCodeDescriptor ProtectedBudgetExceeded{.domain = Domain,
                                                      .code = ErrorCode{"protected_budget_exceeded"},
                                                      .defaultSeverity = ErrorSeverity::Warning,
                                                      .summary = "Active and last-known-good versions exceed the configured disk budget."};
    const ErrorCodeDescriptor UnsafeCleanup{.domain = Domain,
                                            .code = ErrorCode{"unsafe_cleanup"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Installed-version cleanup could not prove ownership or protected pins."};
}  // namespace Horo::Release::UpdateRetentionErrors
