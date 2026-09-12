#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::ShaderCompilerPipelineErrors {
    namespace {
        const ErrorDomainId Domain{"render.shader_compiler"};
    }

    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.invalid_limits", ErrorSeverity::Error,
                                    "Shader compiler pipeline limits are invalid.",
                                    "Provide non-zero finite input and output limits within the engine safety bounds.");
    const ErrorCodeDescriptor InvalidRequest =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.invalid_request", ErrorSeverity::Error,
                                    "The shader compilation request is malformed or incomplete.",
                                    "Provide a valid manifest, non-empty bounded source, and one descriptor for every requested target.");
    const ErrorCodeDescriptor NonCanonicalInput =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.noncanonical_input", ErrorSeverity::Error,
                                    "Shader dependencies, definitions, or targets are duplicated or not canonically ordered.",
                                    "Sort each collection by its stable identity and provide every identity exactly once.");
    const ErrorCodeDescriptor UnsupportedTarget =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.unsupported_target", ErrorSeverity::Error,
                                    "The requested shader route conflicts with the backend's locked baseline.",
                                    "Use the required payload, intermediate environment, layout policy, and target platform contract.");
    const ErrorCodeDescriptor UnpinnedToolchain =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.unpinned_toolchain", ErrorSeverity::Error,
                                    "The shader target does not declare every required exact tool release and build identity.",
                                    "Select host tools explicitly and record their non-empty release plus verified build digest.");
    const ErrorCodeDescriptor CancellationRequested =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.cancelled", ErrorSeverity::Warning,
                                    "The shader compilation batch was cancelled.",
                                    "Retry from the owning preparation operation when the source generation is still current.");
    const ErrorCodeDescriptor AdapterFailure =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.adapter_failed", ErrorSeverity::Error,
                                    "A selected shader toolchain adapter failed.",
                                    "Inspect the preserved adapter cause and target context, then correct the source or host toolchain.");
    const ErrorCodeDescriptor InvalidAdapterOutput =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.invalid_adapter_output", ErrorSeverity::Error,
                                    "A shader toolchain adapter returned a mismatched or unbounded target output.",
                                    "Return the exact requested backend/payload route and keep payloads and diagnostics inside the "
                                    "invocation limits.");
    const ErrorCodeDescriptor ArtifactIdentityUnavailable =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.artifact_identity_unavailable", ErrorSeverity::Error,
                                    "The deterministic shader artifact identity could not be constructed.",
                                    "Reduce the request within its finite limits and retry at a preparation boundary.");
    const ErrorCodeDescriptor AllocationFailed = Detail::
        MakeErrorDescriptor(Domain, "render.shader_compiler.allocation_failed", ErrorSeverity::Error,
                            "The shader compilation batch could not allocate its bounded result storage.",
                            "Reduce the requested target, dependency, payload, or diagnostic bounds and retry the offline operation.");
}  // namespace Horo::Render::ShaderCompilerPipelineErrors
