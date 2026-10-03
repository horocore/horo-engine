#include "Horo/Packages/PackageInstallErrors.h"

namespace Horo::Packages::PackageInstallErrors {
    namespace {
        const ErrorDomainId Domain{"packages.install"};
    }

    const ErrorCodeDescriptor InvalidInput{.domain = Domain,
                                           .code = ErrorCode{"packages.install.invalid_input"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Package install input is invalid.",
                                           .remediationHint = "Use a canonical project root and a bounded complete restore graph."};
    const ErrorCodeDescriptor EvidenceMismatch{.domain = Domain,
                                               .code = ErrorCode{"packages.install.evidence_mismatch"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Package archive evidence differs from the locked graph.",
                                               .remediationHint = "Restore the exact locked packages again before installation."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"packages.install.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Package installation was cancelled before commit.",
                                        .remediationHint = "Retry when the project is ready."};
    const ErrorCodeDescriptor LockUnavailable{.domain = Domain,
                                              .code = ErrorCode{"packages.install.lock_unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The project package install lock is unavailable.",
                                              .remediationHint = "Wait for the other project installation to finish.",
                                              .retryable = true};
    const ErrorCodeDescriptor CommitFailed{.domain = Domain,
                                           .code = ErrorCode{"packages.install.commit_failed"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The project package install record could not be committed.",
                                           .remediationHint = "Check project storage and retry; the previous install remains active.",
                                           .retryable = true};
}  // namespace Horo::Packages::PackageInstallErrors
