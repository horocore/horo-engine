#include "Horo/Runtime/Render/ShaderPermutationErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::ShaderPermutationErrors {
    namespace {
        const ErrorDomainId Domain{"render.shader_permutation"};
    }

    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.invalid_limits", ErrorSeverity::Error,
                                    "Shader permutation limits are invalid.",
                                    "Provide non-zero finite limits within the engine permutation bounds.");
    const ErrorCodeDescriptor InvalidModel =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.invalid_model", ErrorSeverity::Error,
                                    "The shader permutation model is malformed or exceeds its declared bound.",
                                    "Publish a supported finite model whose explicit variants fit the configured cap.");
    const ErrorCodeDescriptor NonCanonicalInput =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.noncanonical_input", ErrorSeverity::Error,
                                    "Shader permutation records are duplicated or not canonically ordered.",
                                    "Sort unique feature bits, masks, and specialization identities before submission.");
    const ErrorCodeDescriptor UnsupportedPermutation =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.unsupported", ErrorSeverity::Error,
                                    "The requested shader feature combination is not an admitted cooked permutation.",
                                    "Request an explicitly declared permutation or publish an authored fallback variant.");
    const ErrorCodeDescriptor InvalidRequest =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.invalid_request", ErrorSeverity::Error,
                                    "The shader permutation request has a missing or malformed logical identity.",
                                    "Provide a valid render pass, vertex layout compatibility digest, and cook target.");
    const ErrorCodeDescriptor InvalidSpecialization =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.invalid_specialization", ErrorSeverity::Error,
                                    "A runtime specialization override is undeclared or has an incompatible value.",
                                    "Use canonical values matching the shader manifest specialization declarations.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.shader_permutation.allocation_failed", ErrorSeverity::Error,
                                    "Shader permutation result storage could not be allocated.",
                                    "Reduce the bounded request and retry at a preparation boundary.");
}  // namespace Horo::Render::ShaderPermutationErrors
