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
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"release.update_transfer.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Update download was cancelled before verification.",
                                        .retryable = true};
    const ErrorCodeDescriptor TransportFailed{.domain = Domain,
                                              .code = ErrorCode{"release.update_transfer.transport_failed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "HTTPS update transfer failed before verification.",
                                              .remediationHint = "Retry from the trusted update source.",
                                              .retryable = true};
    const ErrorCodeDescriptor InvalidCheckpoint{.domain = Domain,
                                                .code = ErrorCode{"release.update_transfer.invalid_checkpoint"},
                                                .defaultSeverity = ErrorSeverity::Warning,
                                                .summary = "Partial update checkpoint is invalid or unsupported.",
                                                .remediationHint = "Discard the partial download and restart from byte zero.",
                                                .userActionable = true};
    const ErrorCodeDescriptor InvalidArchive{.domain = Domain,
                                             .code = ErrorCode{"release.update_transfer.invalid_archive"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Update archive contains unsafe or inconsistent entries.",
                                             .remediationHint = "Discard the package and use a trusted update source.",
                                             .userActionable = true};
    const ErrorCodeDescriptor ArchiveResourceLimit{.domain = Domain,
                                                   .code = ErrorCode{"release.update_transfer.archive_resource_limit"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Update archive exceeds extraction resource limits.",
                                                   .remediationHint =
                                                       "Choose a compatible update package or increase administrator limits.",
                                                   .userActionable = true};
    const ErrorCodeDescriptor StageMismatch{.domain = Domain,
                                            .code = ErrorCode{"release.update_transfer.stage_mismatch"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Extracted update files do not match the authenticated inventory.",
                                            .remediationHint = "Discard the staged version and retry from a trusted package.",
                                            .userActionable = true};
}  // namespace Horo::Release::UpdateTransferErrors
