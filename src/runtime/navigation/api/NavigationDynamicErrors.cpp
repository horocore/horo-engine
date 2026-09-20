#include "Horo/Navigation/NavigationErrors.h"

namespace Horo::Navigation::NavigationErrors {
    namespace {
        const ErrorDomainId NavigationDomain{"horo.navigation"};
    }  // namespace

    const ErrorCodeDescriptor DynamicRegistryInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A dynamic navigation obstacle or modifier registry value is invalid.",
        .remediationHint = "Use finite supported shapes, non-zero identities, exact provenance, and bounded update values.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DynamicRegistryConflict{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A dynamic navigation obstacle or modifier identity conflicts with an active or staged record.",
        .remediationHint = "Use one stable contribution identity and coalesce changes before the next Scene safe point.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DynamicRegistryStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.stale"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "A dynamic navigation update belongs to an old owner, Scene, source, or registry generation.",
        .remediationHint = "Resolve the current Scene-scoped handle and submit the update against its current revision.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DynamicRegistryCapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The dynamic navigation registry exceeded a declared count, command, or publication bound.",
        .remediationHint = "Reduce live contributions or use a profile with a larger explicitly qualified bound.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DynamicRegistryUpdateRateExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.update_rate_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "A dynamic navigation contribution was updated more often than its profile permits.",
        .remediationHint = "Coalesce latest-wins transforms and submit at the next eligible bounded update tick.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DynamicRegistryShuttingDown{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.dynamic_registry.shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Dynamic navigation registry mutation or snapshot admission is closed during teardown.",
        .remediationHint = "Stop submitting dynamic navigation changes after Scene or host shutdown begins.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::Navigation::NavigationErrors
