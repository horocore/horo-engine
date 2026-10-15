#include "Horo/Gameplay/GameplayErrors.h"

namespace Horo::Gameplay::GameplayErrors {
    const ErrorCodeDescriptor CharacterPermissionDenied{
        .domain = ErrorDomainId{"horo.gameplay"},
        .code = ErrorCode{"gameplay.character.permission_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The module has no Character permission for this scene.",
        .remediationHint = "Request an explicit matching module and scene Character grant.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CharacterUnavailable{
        .domain = ErrorDomainId{"horo.gameplay"},
        .code = ErrorCode{"gameplay.character.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Character or its paired Physics dependency is unavailable.",
        .remediationHint = "Activate the exact paired Physics and Character scene worlds before binding gameplay.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Gameplay::GameplayErrors
