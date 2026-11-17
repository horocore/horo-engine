#include "ProjectVersionErrors.h"

namespace Horo::Application::ProjectErrors {
    namespace {
        const ErrorDomainId Domain{"horo.application.project"};
    }

    const ErrorCodeDescriptor VersionInvalid{.domain = Domain,
                                             .code = ErrorCode{"project.version.invalid"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Horo project version is invalid.",
                                             .remediationHint = "Use canonical SemVer without build metadata.",
                                             .retryable = false,
                                             .userActionable = true};

    const ErrorCodeDescriptor HashInvalid{.domain = Domain,
                                          .code = ErrorCode{"project.contract_hash.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Project contract hash is invalid.",
                                          .remediationHint = "Use a canonical lowercase SHA-256 identity.",
                                          .retryable = false,
                                          .userActionable = true};

}  // namespace Horo::Application::ProjectErrors
