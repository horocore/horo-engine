#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    namespace {
        const ErrorDomainId EditorSurfaceContextDomain{"horo.extensions"};
    }

    const ErrorCodeDescriptor EditorSurfaceContextInvalid{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_context_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor-surface context or one of its access lists is malformed.",
        .remediationHint = "Use unique canonical access identities within the configured host bounds.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceContextProviderMismatch{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_context_provider_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor-surface context does not belong to the requested provider activation.",
        .remediationHint = "Attach the surface to its exact extension, module, and activation generation.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorSurfaceContextRevoked{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_context_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The editor-surface context is no longer active.",
        .remediationHint = "Stop using the context and wait for the provider to attach a new activation-scoped surface.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorSurfaceContextCapacityExceeded{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_context_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor-surface context exceeded a finite host bound.",
        .remediationHint = "Reduce the surface access lists or retire an existing provider context.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceContextShutdown{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_context_shutdown"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The editor-surface context provider is shutting down.",
        .remediationHint = "Wait for the next editor/provider activation before attaching the surface.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::Extensions::ExtensionErrors
