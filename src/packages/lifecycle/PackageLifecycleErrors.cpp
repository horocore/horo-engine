#include "Horo/Packages/PackageLifecycleErrors.h"

namespace Horo::Packages::PackageLifecycleErrors {
    namespace {
        const ErrorDomainId Domain{"packages.activation"};
    }

    const ErrorCodeDescriptor InvalidCandidate{.domain = Domain,
                                               .code = ErrorCode{"packages.activation.invalid_candidate"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Package activation candidate is invalid.",
                                               .remediationHint =
                                                   "Select declared compatible descriptors and artifacts from one installed graph."};
    const ErrorCodeDescriptor TrustRequired{.domain = Domain,
                                            .code = ErrorCode{"packages.activation.trust_required"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Exact package execution trust is required.",
                                            .remediationHint =
                                                "Obtain local or organization approval for the exact installed bytes and capabilities."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"packages.activation.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Package activation was cancelled before publication.",
                                        .remediationHint = "Retry activation at a host safe point."};
    const ErrorCodeDescriptor StaleInstall{.domain = Domain,
                                           .code = ErrorCode{"packages.activation.stale_install"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Installed package composition changed during activation.",
                                           .remediationHint = "Retry using the current install record."};
    const ErrorCodeDescriptor
        InvalidLifecycle{.domain = Domain,
                         .code = ErrorCode{"packages.activation.invalid_lifecycle"},
                         .defaultSeverity = ErrorSeverity::Error,
                         .summary = "Package activation is unavailable at this lifecycle boundary.",
                         .remediationHint =
                             "Use the composition owner lane at startup or after quiescing consumers; drain retired owners first."};
    const ErrorCodeDescriptor StorageFailed{.domain = Domain,
                                            .code = ErrorCode{"packages.activation.storage_failed"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Verified native package content could not be materialized.",
                                            .remediationHint = "Check host temporary storage permissions and capacity."};
}  // namespace Horo::Packages::PackageLifecycleErrors
