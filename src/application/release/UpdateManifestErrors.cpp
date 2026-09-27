#include "Horo/Release/UpdateManifestErrors.h"

namespace Horo::Release::UpdateManifestErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update"};
    }

    const ErrorCodeDescriptor Invalid{.domain = Domain,
                                      .code = ErrorCode{"release.update_manifest.invalid"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "Update metadata is malformed or has invalid byte evidence.",
                                      .remediationHint = "Obtain canonical signed metadata from an authorized source.",
                                      .userActionable = true};
    const ErrorCodeDescriptor Stale{.domain = Domain,
                                    .code = ErrorCode{"release.update_manifest.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "Update metadata is expired or older than installed trust state.",
                                    .remediationHint = "Refresh signed metadata or use an approved recovery policy.",
                                    .userActionable = true};
    const ErrorCodeDescriptor Incompatible{.domain = Domain,
                                           .code = ErrorCode{"release.update_manifest.incompatible"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Update metadata does not match this product or target.",
                                           .remediationHint = "Select metadata for the installed product and target.",
                                           .userActionable = true};
    const ErrorCodeDescriptor Rollback{.domain = Domain,
                                       .code = ErrorCode{"release.update_manifest.rollback"},
                                       .defaultSeverity = ErrorSeverity::Error,
                                       .summary = "Update version violates downgrade or anti-rollback policy.",
                                       .remediationHint = "Use a newer authorized update or an explicit recovery operation.",
                                       .userActionable = true};
}  // namespace Horo::Release::UpdateManifestErrors
