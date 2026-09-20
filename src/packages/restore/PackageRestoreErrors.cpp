#include "Horo/Packages/PackageRestoreErrors.h"

namespace Horo::Packages::PackageRestoreErrors {
    namespace {
        const ErrorDomainId Domain{"packages.restore"};
    }

    const ErrorCodeDescriptor InvalidInput{.domain = Domain,
                                           .code = ErrorCode{"packages.restore.invalid_input"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Package restore input is invalid.",
                                           .remediationHint = "Provide a bounded canonical project request and restore platform."};
    const ErrorCodeDescriptor ResourceLimit{.domain = Domain,
                                            .code = ErrorCode{"packages.restore.resource_limit"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Package restore input exceeds its resource policy.",
                                            .remediationHint = "Reduce lockfile or artifact size, or raise the trusted host limit."};
    const ErrorCodeDescriptor Busy{.domain = Domain,
                                   .code = ErrorCode{"packages.restore.busy"},
                                   .defaultSeverity = ErrorSeverity::Error,
                                   .summary = "Another package restore is already active.",
                                   .remediationHint = "Wait for the active restore to finish before retrying.",
                                   .retryable = true,
                                   .userActionable = true};
    const ErrorCodeDescriptor LockUnavailable{.domain = Domain,
                                              .code = ErrorCode{"packages.restore.lock_unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The project restore lock could not be acquired.",
                                              .remediationHint = "Close competing project operations and check project permissions.",
                                              .retryable = true,
                                              .userActionable = true};
    const ErrorCodeDescriptor LifecycleClosed{.domain = Domain,
                                              .code = ErrorCode{"packages.restore.lifecycle_closed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Package restore service is closed.",
                                              .remediationHint = "Create a new host-owned package restore service."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"packages.restore.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Package restore was cancelled.",
                                        .remediationHint = "Retry restore when the project is ready."};
    const ErrorCodeDescriptor LockfileReadFailed{.domain = Domain,
                                                 .code = ErrorCode{"packages.restore.lockfile_read_failed"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The project package lockfile could not be read.",
                                                 .remediationHint = "Restore or regenerate .horo/packages.lock and check its permissions.",
                                                 .retryable = true,
                                                 .userActionable = true};
    const ErrorCodeDescriptor SourceUnavailable{.domain = Domain,
                                                .code = ErrorCode{"packages.restore.source_unavailable"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "A required package source is unavailable.",
                                                .remediationHint =
                                                    "Check the configured source or provide the artifact through a local mirror.",
                                                .retryable = true,
                                                .userActionable = true};
    const ErrorCodeDescriptor OfflineArtifactUnavailable{.domain = Domain,
                                                         .code = ErrorCode{"packages.restore.offline_artifact_unavailable"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "A required package artifact is unavailable offline.",
                                                         .remediationHint = "Vendor the exact locked artifact or use an online restore.",
                                                         .userActionable = true};
    const ErrorCodeDescriptor ArtifactHashMismatch{.domain = Domain,
                                                   .code = ErrorCode{"packages.restore.artifact_hash_mismatch"},
                                                   .defaultSeverity = ErrorSeverity::Critical,
                                                   .summary = "A package artifact does not match its lockfile digest.",
                                                   .remediationHint = "Quarantine the artifact and obtain the exact locked bytes.",
                                                   .userActionable = true};
    const ErrorCodeDescriptor ArtifactInvalid{.domain = Domain,
                                              .code = ErrorCode{"packages.restore.artifact_invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "A package artifact failed validation.",
                                              .remediationHint = "Rebuild or replace the package with a complete verified archive.",
                                              .userActionable = true};
    const ErrorCodeDescriptor ArtifactEvidenceMismatch{.domain = Domain,
                                                       .code = ErrorCode{"packages.restore.artifact_evidence_mismatch"},
                                                       .defaultSeverity = ErrorSeverity::Critical,
                                                       .summary = "A package archive does not match lockfile evidence.",
                                                       .remediationHint =
                                                           "Regenerate the lockfile from the exact verified package artifact.",
                                                       .userActionable = true};
    const ErrorCodeDescriptor PublisherRejected{.domain = Domain,
                                                .code = ErrorCode{"packages.restore.publisher_rejected"},
                                                .defaultSeverity = ErrorSeverity::Critical,
                                                .summary = "Publisher policy rejected a package artifact.",
                                                .remediationHint = "Use an artifact signed by an approved, current publisher key.",
                                                .userActionable = true};
    const ErrorCodeDescriptor PublisherEvidenceUnavailable{.domain = Domain,
                                                           .code = ErrorCode{"packages.restore.publisher_evidence_unavailable"},
                                                           .defaultSeverity = ErrorSeverity::Critical,
                                                           .summary = "Publisher evidence is unavailable for a required cached artifact.",
                                                           .remediationHint =
                                                               "Restore the artifact from a source that supplies its detached signature.",
                                                           .retryable = true,
                                                           .userActionable = true};
    const ErrorCodeDescriptor CacheFailure{.domain = Domain,
                                           .code = ErrorCode{"packages.restore.cache_failed"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Package cache access failed during restore.",
                                           .remediationHint = "Check cache permissions, storage, and quarantine diagnostics.",
                                           .retryable = true,
                                           .userActionable = true};
    const ErrorCodeDescriptor QuarantineFailed{.domain = Domain,
                                               .code = ErrorCode{"packages.restore.quarantine_failed"},
                                               .defaultSeverity = ErrorSeverity::Critical,
                                               .summary = "A failed package artifact could not be quarantined.",
                                               .remediationHint = "Repair cache storage permissions before retrying restore.",
                                               .retryable = true,
                                               .userActionable = true};
}  // namespace Horo::Packages::PackageRestoreErrors
