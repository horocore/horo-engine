#include "Horo/Runtime/Render/PipelineCacheErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::PipelineCacheErrors {
    namespace {
        const ErrorDomainId Domain{"render.pipeline_cache"};
    }

    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.invalid_limits", ErrorSeverity::Error,
                                    "Pipeline cache limits are invalid.", "Provide finite non-zero limits within the hard bounds.");
    const ErrorCodeDescriptor InvalidCompatibility = Detail::
        MakeErrorDescriptor(Domain, "render.pipeline_cache.invalid_compatibility", ErrorSeverity::Error,
                            "Pipeline cache compatibility identity is incomplete or malformed.",
                            "Provide exact backend, module, adapter, driver, shader, interface, and pipeline descriptor identities.");
    const ErrorCodeDescriptor IdentityUnavailable =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.identity_unavailable", ErrorSeverity::Error,
                                    "Pipeline cache identity could not be constructed.",
                                    "Reduce bounded identity input and retry outside frame-hot execution.");
    const ErrorCodeDescriptor PayloadTooLarge =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.payload_too_large", ErrorSeverity::Error,
                                    "Pipeline cache payload exceeds the admitted bound.",
                                    "Discard the optional native cache blob and rebuild from the cooked shader artifact.");
    const ErrorCodeDescriptor UnsupportedVersion =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.unsupported_version", ErrorSeverity::Error,
                                    "Pipeline cache blob schema version is unsupported.",
                                    "Discard the stale optional blob and rebuild it with the current backend.");
    const ErrorCodeDescriptor CorruptBlob =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.corrupt_blob", ErrorSeverity::Error,
                                    "Pipeline cache blob structure or payload integrity is invalid.",
                                    "Discard the corrupt optional blob and rebuild from the cooked shader artifact.");
    const ErrorCodeDescriptor IncompatibleBlob =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.incompatible_blob", ErrorSeverity::Warning,
                                    "Pipeline cache blob belongs to a different compatibility identity.",
                                    "Discard the stale optional blob and rebuild for the active backend, device, driver, and shader.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_cache.allocation_failed", ErrorSeverity::Error,
                                    "Pipeline cache payload storage could not be allocated.",
                                    "Discard the optional blob or reduce its admitted size before retrying.");
}  // namespace Horo::Render::PipelineCacheErrors
