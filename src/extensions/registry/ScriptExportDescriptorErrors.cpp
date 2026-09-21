#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    namespace {
        const ErrorDomainId ScriptExportDescriptorDomain{"horo.extensions"};
    }

    const ErrorCodeDescriptor ScriptExportDescriptorInvalid{
        .domain = ScriptExportDescriptorDomain,
        .code = ErrorCode{"script_export_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The language-neutral script export descriptor is malformed or inconsistent.",
        .remediationHint = "Check stable identities, typed references, bounds, nullability, and service ownership.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptExportDescriptorConflict{
        .domain = ScriptExportDescriptorDomain,
        .code = ErrorCode{"script_export_descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Script export identities conflict within one descriptor generation.",
        .remediationHint = "Use unique stable API, type, member, constant, error, and tombstone identities.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptExportDescriptorIncompatible{
        .domain = ScriptExportDescriptorDomain,
        .code = ErrorCode{"script_export_descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script export replacement is not compatible with its prior generation.",
        .remediationHint = "Use additive same-major evolution or publish new identities and a breaking major version.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptExportDescriptorCapacityExceeded{
        .domain = ScriptExportDescriptorDomain,
        .code = ErrorCode{"script_export_descriptor_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script export descriptor exceeded an explicit host bound.",
        .remediationHint = "Reduce declaration counts, documentation, type depth, or canonical value sizes.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Extensions::ExtensionErrors
