#include "Horo/Release/UpdateActivationErrors.h"

namespace Horo::Release::UpdateActivationErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update.activation"};
    }

    const ErrorCodeDescriptor InvalidLayout{.domain = Domain,
                                            .code = ErrorCode{"invalid_layout"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The update activation layout or version identities are invalid."};
    const ErrorCodeDescriptor CurrentMismatch{.domain = Domain,
                                              .code = ErrorCode{"current_mismatch"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The active version differs from the verified current version."};
    const ErrorCodeDescriptor PendingMismatch{.domain = Domain,
                                              .code = ErrorCode{"pending_mismatch"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "An interrupted activation cannot be recovered with this version evidence."};
    const ErrorCodeDescriptor HealthFailed{.domain = Domain,
                                           .code = ErrorCode{"health_failed"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The staged version failed its startup health check and was rolled back."};
    const ErrorCodeDescriptor RollbackFailed{.domain = Domain,
                                             .code = ErrorCode{"rollback_failed"},
                                             .defaultSeverity = ErrorSeverity::Critical,
                                             .summary = "The previous active version could not be restored; recovery is required."};
}  // namespace Horo::Release::UpdateActivationErrors
