#include "Horo/Release/UpdateOfflineSourceErrors.h"

namespace Horo::Release::UpdateOfflineSourceErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update"};
    }

    const ErrorCodeDescriptor InvalidPolicy{.domain = Domain,
                                            .code = ErrorCode{"release.update_source.invalid_policy"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Managed update source policy is invalid.",
                                            .remediationHint = "Correct the source identity, channel, or precedence settings.",
                                            .userActionable = true};
    const ErrorCodeDescriptor Unavailable{.domain = Domain,
                                          .code = ErrorCode{"release.update_source.unavailable"},
                                          .defaultSeverity = ErrorSeverity::Warning,
                                          .summary = "Configured update media or metadata is unavailable.",
                                          .remediationHint = "Connect the media or choose another configured source.",
                                          .retryable = true};
    const ErrorCodeDescriptor UnsafePath{.domain = Domain,
                                         .code = ErrorCode{"release.update_source.unsafe_path"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "Update source path leaves its configured root or contains a link.",
                                         .remediationHint = "Use a regular source directory with no linked files.",
                                         .userActionable = true};
    const ErrorCodeDescriptor MirrorMismatch{.domain = Domain,
                                             .code = ErrorCode{"release.update_source.mirror_mismatch"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Local package differs from the signed update manifest.",
                                             .remediationHint = "Repair the mirror or obtain the signed package again.",
                                             .userActionable = true};
    const ErrorCodeDescriptor NoUpdate{.domain = Domain,
                                       .code = ErrorCode{"release.update_source.no_update"},
                                       .defaultSeverity = ErrorSeverity::Info,
                                       .summary = "Source has no newer compatible ZIP update.",
                                       .userActionable = false};
}  // namespace Horo::Release::UpdateOfflineSourceErrors
