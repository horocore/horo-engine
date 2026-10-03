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

    const ErrorCodeDescriptor EditorSurfaceRegistryInvalid{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The external editor-surface registry input is malformed.",
        .remediationHint = "Use a validated persistent panel or tab descriptor and bounded workspace state.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryDuplicate{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The external editor-surface contribution is already registered.",
        .remediationHint = "Withdraw the old contribution or use a globally unique surface identity.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryCapacityExceeded{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The external editor-surface registry reached a finite host bound.",
        .remediationHint = "Close or withdraw a contribution before registering another surface.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryShutdown{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "External editor-surface admission is closed.",
        .remediationHint = "Wait for the next editor session/provider activation.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryUnknown{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested external editor surface is not registered.",
        .remediationHint = "Use a live contribution identity from the current editor session.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryProviderDisabled{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_provider_disabled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The external editor-surface provider is disabled.",
        .remediationHint = "Enable the provider or keep the bounded presentation state deferred.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryProviderMissing{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_provider_missing"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The external editor-surface provider is missing or unloaded.",
        .remediationHint = "Restore the provider before opening or focusing the surface.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistrySurfaceClosed{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_surface_closed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The external editor surface is closed.",
        .remediationHint = "Open the surface before requesting focus.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceRegistryStateInvalid{
        .domain = EditorSurfaceContextDomain,
        .code = ErrorCode{"editor_surface_registry_state_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "External editor-surface workspace state is invalid or oversized.",
        .remediationHint = "Discard the malformed state and restore only bounded presentation data.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Extensions::ExtensionErrors
