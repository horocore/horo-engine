#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    namespace {
        const ErrorDomainId EditorCommandDomain{"horo.extensions"};
    }

    const ErrorCodeDescriptor EditorCommandInvalid{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor command contribution is malformed or outside its surface contract.",
        .remediationHint =
            "Use a canonical ID, allowlisted localization, valid predicates, and a supported menu, toolbar, or status surface.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorCommandDuplicate{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor command identity is already published.",
        .remediationHint = "Choose one globally stable command identity for the active host composition.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorCommandShortcutConflict{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_shortcut_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor command shortcut conflicts with another published command.",
        .remediationHint = "Choose a distinct shortcut or omit the shortcut and let host policy assign one.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorCommandCapacityExceeded{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded editor-command registry capacity was exhausted.",
        .remediationHint = "Retire an existing contribution or reduce the active extension composition.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorCommandUnknown{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested editor command is not published.",
        .remediationHint = "Refresh the command snapshot and use a live stable command identity.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorCommandNotEnabled{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_not_enabled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The requested editor command is disabled for the current editor state.",
        .remediationHint = "Re-evaluate the command after its required project, selection, surface, or capability state is available.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorCommandProviderRevoked{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_provider_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The editor command provider activation is no longer usable.",
        .remediationHint = "Discard the invocation and wait for the current extension activation to publish a replacement.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorCommandShutdown{
        .domain = EditorCommandDomain,
        .code = ErrorCode{"editor_command_shutdown"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Editor-command publication is closed for this host registry.",
        .remediationHint = "Attach contributions to the next editor host composition.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::Extensions::ExtensionErrors
