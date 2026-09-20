#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};
    }  // namespace

    const ErrorCodeDescriptor NetworkProjectSettingsInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.project_settings.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network project settings are malformed or incoherent.",
        .remediationHint = "Provide one complete versioned project policy with valid roles, budgets, protocol, and transport requirements.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkProjectSettingsCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.project_settings.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network project settings exceed an explicit finite capacity.",
        .remediationHint = "Lower the requested profile, queue, connection, or scheduling limits within the admitted bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkProjectSettingsStale{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.project_settings.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The network project-settings revision is stale.",
        .remediationHint = "Reload the current immutable project snapshot and submit a complete successor revision.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor NetworkProjectSettingsShuttingDown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.project_settings.shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Network project-settings publication is closed.",
        .remediationHint = "Discard late settings commands and create a new owner generation before publishing again.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::Network::NetworkErrors
