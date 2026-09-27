#include "Horo/Release/UpdateTransferErrors.h"

namespace Horo::Release::UpdateTransferErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update"};
    }

    const ErrorCodeDescriptor InsufficientSpace{.domain = Domain,
                                                .code = ErrorCode{"release.update_transfer.insufficient_space"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Update package exceeds private storage capacity or policy.",
                                                .remediationHint = "Free storage space or choose another update source.",
                                                .userActionable = true};
    const ErrorCodeDescriptor InvalidResponse{.domain = Domain,
                                              .code = ErrorCode{"release.update_transfer.invalid_response"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Update source returned an invalid package response.",
                                              .remediationHint = "Retry from a trusted update source.",
                                              .userActionable = true};
    const ErrorCodeDescriptor ResumeMismatch{.domain = Domain,
                                             .code = ErrorCode{"release.update_transfer.resume_mismatch"},
                                             .defaultSeverity = ErrorSeverity::Warning,
                                             .summary = "Partial update bytes no longer match the source validator.",
                                             .remediationHint = "Discard the partial download and restart from byte zero.",
                                             .userActionable = true};
}  // namespace Horo::Release::UpdateTransferErrors
