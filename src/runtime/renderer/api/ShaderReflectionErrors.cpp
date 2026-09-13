#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::ShaderReflectionErrors {
    namespace {
        const ErrorDomainId Domain{"render.shader_reflection"};
    }

    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.invalid_limits", ErrorSeverity::Error,
                                    "Shader reflection validation limits are invalid.",
                                    "Provide non-zero finite limits within the engine reflection bounds.");
    const ErrorCodeDescriptor InvalidReflection =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.invalid", ErrorSeverity::Error,
                                    "Final shader reflection is malformed or not canonically ordered.",
                                    "Emit bounded records in canonical logical-identity and stage-location order.");
    const ErrorCodeDescriptor ManifestMismatch = Detail::
        MakeErrorDescriptor(Domain, "render.shader_reflection.manifest_mismatch", ErrorSeverity::Error,
                            "Final shader reflection disagrees with the declared logical interface.",
                            "Reflect the final target artifact and preserve every declared logical record, including inactive ones.");
    const ErrorCodeDescriptor TargetMappingInvalid =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.target_mapping_invalid", ErrorSeverity::Error,
                                    "The final target binding map is incomplete or inconsistent.",
                                    "Provide one canonical target record per logical resource and all required runtime lookup evidence.");
    const ErrorCodeDescriptor NativeBindingCollision =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.native_binding_collision", ErrorSeverity::Error,
                                    "Multiple active resources occupy the same native target binding.",
                                    "Assign unique native positions within each target resource namespace.");
    const ErrorCodeDescriptor LayoutMismatch = Detail::
        MakeErrorDescriptor(Domain, "render.shader_reflection.layout_mismatch", ErrorSeverity::Error,
                            "Final parameter packing is invalid or overlaps another parameter.",
                            "Emit target-derived offsets and strides that match the declared parameter types and finite buffer bounds.");
    const ErrorCodeDescriptor StageInterfaceMismatch =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.stage_interface_mismatch", ErrorSeverity::Error,
                                    "Final shader stage inputs and outputs do not link exactly.",
                                    "Match every fragment input to the vertex output at the same location and type shape.");
    const ErrorCodeDescriptor SourceMapInvalid = Detail::
        MakeErrorDescriptor(Domain, "render.shader_reflection.source_map_invalid", ErrorSeverity::Error,
                            "Shader source-map records are malformed, overlapping, or lack source provenance.",
                            "Emit canonical non-overlapping generated ranges with admitted source identities and positive line numbers.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.shader_reflection.allocation_failed", ErrorSeverity::Error,
                                    "Normalized shader reflection storage could not be allocated.",
                                    "Reduce reflection data within the configured finite limits and retry outside frame-hot execution.");
}  // namespace Horo::Render::ShaderReflectionErrors
