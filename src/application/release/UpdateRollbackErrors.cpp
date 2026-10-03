#include "Horo/Release/UpdateRollbackErrors.h"

namespace Horo::Release::UpdateRollbackErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update.rollback"};
    }

    const ErrorCodeDescriptor InvalidTarget{.domain = Domain,
                                            .code = ErrorCode{"invalid_target"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Rollback target must be a retained prior version of the installed product."};
    const ErrorCodeDescriptor PolicyDenied{.domain = Domain,
                                           .code = ErrorCode{"policy_denied"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Rollback target is below the authorized version floor."};
    const ErrorCodeDescriptor ConfirmationRequired{.domain = Domain,
                                                   .code = ErrorCode{"confirmation_required"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Explicit rollback requires acknowledgement of the downgrade warning."};
}  // namespace Horo::Release::UpdateRollbackErrors
