#include "Horo/Gameplay/GameplayErrors.h"

namespace Horo::Gameplay::GameplayErrors {
    namespace {
        const ErrorDomainId GameplayDomain{"horo.gameplay"};
    }

    const ErrorCodeDescriptor ComponentDescriptorMissing{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_descriptor_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene references a gameplay component whose descriptor is unavailable.",
        .remediationHint = "Restore the compatible project gameplay module before entering Play Mode; the opaque payload is preserved.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ComponentSchemaIncompatible{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.component_schema_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene gameplay component schema is incompatible with the active module.",
        .remediationHint = "Load a compatible module or explicitly replace the component through the typed repair command.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor GameplayPlayBlocked{
        .domain = GameplayDomain,
        .code = ErrorCode{"gameplay.play_blocked"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Play Mode is blocked by unavailable or incompatible gameplay data.",
        .remediationHint =
            "Restore a compatible gameplay module or repair the affected authored components without discarding their payloads.",
        .retryable = true,
        .userActionable = true,
    };
}  // namespace Horo::Gameplay::GameplayErrors
