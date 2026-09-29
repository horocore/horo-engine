#include "Horo/Release/UserStateMigrationErrors.h"

namespace Horo::Release::UserStateMigrationErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.user_state_migration"};
    }

    const ErrorCodeDescriptor InvalidPlan{.domain = Domain,
                                          .code = ErrorCode{"invalid_plan"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "User-state migration plan is invalid."};
    const ErrorCodeDescriptor SourceChanged{.domain = Domain,
                                            .code = ErrorCode{"source_changed"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "User-state source differs from the planned version."};
    const ErrorCodeDescriptor BackupRequiresRepair{.domain = Domain,
                                                   .code = ErrorCode{"backup_requires_repair"},
                                                   .defaultSeverity = ErrorSeverity::Warning,
                                                   .summary = "A prior migration backup needs explicit repair or review."};
    const ErrorCodeDescriptor TransformFailed{.domain = Domain,
                                              .code = ErrorCode{"transform_failed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "User-state transformation failed or produced unexpected bytes."};
    const ErrorCodeDescriptor UnsafePath{.domain = Domain,
                                         .code = ErrorCode{"unsafe_path"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "User-state path is not a private regular file."};
}  // namespace Horo::Release::UserStateMigrationErrors
