#include "Horo/Release/BootstrapInstallationErrors.h"

namespace Horo::Release::BootstrapInstallationErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.bootstrap"};
    }

    const ErrorCodeDescriptor InvalidLayout{.domain = Domain,
                                            .code = ErrorCode{"invalid_layout"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "The first-install package or destination layout is invalid."};
    const ErrorCodeDescriptor AlreadyInstalled{.domain = Domain,
                                               .code = ErrorCode{"already_installed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "An active installation already exists at this destination."};
    const ErrorCodeDescriptor PendingMismatch{.domain = Domain,
                                              .code = ErrorCode{"pending_mismatch"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "An interrupted first install does not match the candidate."};
    const ErrorCodeDescriptor PreflightFailed{.domain = Domain,
                                              .code = ErrorCode{"preflight_failed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The host cannot install this package at the selected destination."};
    const ErrorCodeDescriptor IntegrationFailed{.domain = Domain,
                                                .code = ErrorCode{"integration_failed"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Operating-system registration failed during first install."};
    const ErrorCodeDescriptor HealthFailed{.domain = Domain,
                                           .code = ErrorCode{"health_failed"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "First-launch health failed and the candidate was deactivated."};
    const ErrorCodeDescriptor RecoveryFailed{.domain = Domain,
                                             .code = ErrorCode{"recovery_failed"},
                                             .defaultSeverity = ErrorSeverity::Critical,
                                             .summary = "First-install state could not be safely undone."};
    const ErrorCodeDescriptor RepairFailed{.domain = Domain,
                                           .code = ErrorCode{"repair_failed"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The installed product could not be repaired or proven healthy."};
    const ErrorCodeDescriptor UninstallFailed{.domain = Domain,
                                              .code = ErrorCode{"uninstall_failed"},
                                              .defaultSeverity = ErrorSeverity::Critical,
                                              .summary = "Uninstall is incomplete and must be retried with the same package."};
}  // namespace Horo::Release::BootstrapInstallationErrors
