#include "Horo/Gameplay/GameplayVfxBindingTable.h"

namespace Horo::Gameplay::GameplayVfxErrors {
    namespace {
        const ErrorDomainId Domain{"horo.gameplay.vfx"};
    }

    const ErrorCodeDescriptor Malformed{Domain, ErrorCode{"gameplay.vfx.binding.malformed"}, ErrorSeverity::Error,
                                        "The gameplay VFX binding source is malformed.",
                                        "Use the bounded canonical version-one binding schema."};
    const ErrorCodeDescriptor DuplicateField{Domain, ErrorCode{"gameplay.vfx.binding.duplicate_field"}, ErrorSeverity::Error,
                                             "The gameplay VFX source contains an ambiguous duplicate field.",
                                             "Remove duplicate object fields."};
    const ErrorCodeDescriptor VersionUnsupported{Domain, ErrorCode{"gameplay.vfx.binding.version_unsupported"}, ErrorSeverity::Error,
                                                 "The gameplay VFX binding schema version is unsupported.",
                                                 "Migrate the source explicitly before loading it."};
    const ErrorCodeDescriptor LimitExceeded{Domain, ErrorCode{"gameplay.vfx.binding.limit_exceeded"}, ErrorSeverity::Error,
                                            "The gameplay VFX binding source exceeds a finite preparation limit.",
                                            "Reduce source bytes, nodes, nesting or authored fan-out."};
    const ErrorCodeDescriptor BindingInvalid{Domain, ErrorCode{"gameplay.vfx.binding.invalid"}, ErrorSeverity::Error,
                                             "The gameplay VFX binding identities or policies conflict.",
                                             "Declare unique canonical events/tags, destinations and bounded layers."};
    const ErrorCodeDescriptor MissingAsset{Domain, ErrorCode{"gameplay.vfx.asset.missing"}, ErrorSeverity::Error,
                                           "A mapped effect asset is absent from the captured registry.",
                                           "Prepare the declared effect in the same captured asset inventory."};
    const ErrorCodeDescriptor AssetTypeMismatch{Domain, ErrorCode{"gameplay.vfx.asset.type_mismatch"}, ErrorSeverity::Error,
                                                "A mapped asset is not a core.vfx.effect record.", "Map an explicitly typed effect asset."};
    const ErrorCodeDescriptor AssetUnavailable{Domain, ErrorCode{"gameplay.vfx.asset.unavailable"}, ErrorSeverity::Error,
                                               "A mapped effect has no available admitted payload or prepared recipe.",
                                               "Load and admit the effect before publishing the table."};
    const ErrorCodeDescriptor ParameterMismatch{Domain, ErrorCode{"gameplay.vfx.parameter.mismatch"}, ErrorSeverity::Error,
                                                "A mapped scalar schema or range does not match its genuine prepared effect.",
                                                "Use declared source and effect GameplayInput channels and ranges."};
    const ErrorCodeDescriptor ScopeInvalid{Domain, ErrorCode{"gameplay.vfx.scope.invalid"}, ErrorSeverity::Error,
                                           "The gameplay VFX scope does not match its live application owner.",
                                           "Compose a fresh exact-Scene authorized scope."};
    const ErrorCodeDescriptor AllocationFailed{Domain, ErrorCode{"gameplay.vfx.preparation.allocation_failed"}, ErrorSeverity::Error,
                                               "Cold gameplay VFX preparation could not allocate its bounded storage.",
                                               "Preserve active ownership and retry at an explicit preparation boundary."};
    const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"gameplay.vfx.preparation.cancelled"}, ErrorSeverity::Warning,
                                        "Cold gameplay VFX preparation was cancelled before publication.",
                                        "Keep the active binding generation and discard the cancelled candidate."};
}  // namespace Horo::Gameplay::GameplayVfxErrors
