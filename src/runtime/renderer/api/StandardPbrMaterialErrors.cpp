#include "Horo/Runtime/Render/StandardPbrMaterialErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::StandardPbrMaterialErrors {
    namespace {
        const ErrorDomainId Domain{"render.standard_pbr_material"};
    }

    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_material.invalid_limits", ErrorSeverity::Error,
                                    "Standard PBR material limits are invalid.",
                                    "Provide non-zero finite texture and parameter-byte bounds within the engine limits.");
    const ErrorCodeDescriptor InvalidDescriptor = Detail::
        MakeErrorDescriptor(Domain, "render.standard_pbr_material.invalid_descriptor", ErrorSeverity::Error,
                            "The standard PBR material descriptor is invalid.",
                            "Provide finite normalized values, a valid identity and revision, and a coherent alpha and quality policy.");
    const ErrorCodeDescriptor UnsupportedQuality = Detail::
        MakeErrorDescriptor(Domain, "render.standard_pbr_material.unsupported_quality", ErrorSeverity::Error,
                            "The selected material quality or required feature set is unavailable.",
                            "Select the preferred profile or an authored fallback whose effective features satisfy every requirement.");
    const ErrorCodeDescriptor InvalidResidentResource = Detail::
        MakeErrorDescriptor(Domain, "render.standard_pbr_material.invalid_resident_resource", ErrorSeverity::Error,
                            "A resident material resource identity is invalid.",
                            "Provide one non-zero material generation and valid generation-safe pipeline, texture, and sampler handles.");
    const ErrorCodeDescriptor TextureBindingMismatch =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_material.texture_binding_mismatch", ErrorSeverity::Error,
                                    "Standard PBR texture bindings do not match the declared feature set.",
                                    "Provide each enabled semantic texture role exactly once and omit roles whose feature is disabled.");
    const ErrorCodeDescriptor ReflectionMismatch =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_material.reflection_mismatch", ErrorSeverity::Error,
                                    "Final shader reflection cannot pack the standard PBR parameter contract.",
                                    "Publish active float parameters with the canonical logical IDs, shapes, binding, offsets, and target "
                                    "strides.");
    const ErrorCodeDescriptor ParameterBufferTooLarge =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_material.parameter_buffer_too_large", ErrorSeverity::Error,
                                    "The reflected standard PBR parameter block exceeds its admitted byte bound.",
                                    "Reduce the target layout or increase the host-composed finite bound within the hard engine limit.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_material.allocation_failed", ErrorSeverity::Error,
                                    "Resident standard PBR material storage could not be allocated.",
                                    "Reduce bounded material data and retry outside frame-hot execution.");
}  // namespace Horo::Render::StandardPbrMaterialErrors
