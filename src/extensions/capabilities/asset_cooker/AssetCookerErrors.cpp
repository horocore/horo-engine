#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.extensions"};
    }

    const ErrorCodeDescriptor AssetCookerRegistryInvalid{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The asset-cooker registry input is invalid.",
        .remediationHint = "Provide canonical provider metadata, typed targets, deterministic versions, and bounded input.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AssetCookerRegistryDuplicate{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The asset-cooker contribution identity is already registered.",
        .remediationHint = "Publish each stable cooker contribution identity once per host composition.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AssetCookerRegistryCapacityExceeded{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The asset-cooker provider registry capacity was exceeded.",
        .remediationHint = "Reduce the explicitly composed cooker provider set.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AssetCookerRegistryShutdown{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The asset-cooker registry is shutting down.",
        .remediationHint = "Do not register or invoke cookers after host shutdown begins.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AssetCookerUnavailable{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "No asset cooker is available for the requested type and target.",
        .remediationHint = "Install or enable a cooker that explicitly declares the requested target.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetCookerAmbiguous{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_ambiguous"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple asset cookers claim the requested type and target.",
        .remediationHint = "Select one exact contribution ID through project policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetCookerOutputInvalid{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_output_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The asset cooker produced invalid staged output.",
        .remediationHint = "Fix the provider to emit one bounded payload with unique dependencies and valid diagnostics.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AssetCookerInvocationFailed{
        .domain = kDomain,
        .code = ErrorCode{"asset_cooker_invocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected asset-cooker provider failed.",
        .remediationHint = "Inspect the attributed provider error and cooker diagnostics.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetCookCancelled{
        .domain = kDomain,
        .code = ErrorCode{"asset_cook_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The asset cook was cancelled.",
        .remediationHint = "Retry the cook when cancellation is no longer requested.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::Extensions::ExtensionErrors
