#include "Horo/Gameplay/GameplayErrors.h"

namespace Horo::Gameplay::GameplayErrors {
    const ErrorCodeDescriptor PhysicsPermissionDenied{
        .domain = ErrorDomainId{"horo.gameplay"},
        .code = ErrorCode{"gameplay.physics.permission_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The module has no Physics query/event permission.",
        .remediationHint = "Request an explicit host grant for this module and world.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PhysicsUnavailable{
        .domain = ErrorDomainId{"horo.gameplay"},
        .code = ErrorCode{"gameplay.physics.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics is disabled or unavailable for this execution context.",
        .remediationHint = "Activate an explicitly selected compatible Physics world before binding gameplay.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Gameplay::GameplayErrors
