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
    const ErrorCodeDescriptor ToolchainConfigurationInvalid =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.toolchain_configuration_invalid", ErrorSeverity::Error,
                                    "The host shader-toolchain configuration is malformed.",
                                    "Provide an absolute scratch root, finite process bounds, and sorted unique approved tools.");
    const ErrorCodeDescriptor ToolNotApproved =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.tool_not_approved", ErrorSeverity::Error,
                                    "A configured shader tool is not present in the reviewed host lock catalog.",
                                    "Install an exact release and host artifact recorded in the shader-toolchain lock catalog.");
    const ErrorCodeDescriptor ToolMissing =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.tool_missing", ErrorSeverity::Error,
                                    "A required shader-tool executable is absent or unreadable.",
                                    "Configure the reviewed executable for this cook host or submit the target to a qualified build host.");
    const ErrorCodeDescriptor ToolDigestMismatch =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.tool_digest_mismatch", ErrorSeverity::Error,
                                    "A shader-tool executable does not match its reviewed content digest.",
                                    "Replace the executable with the exact locked artifact; do not continue with an ambient SDK default.");
    const ErrorCodeDescriptor ScratchIoFailed =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.scratch_io_failed", ErrorSeverity::Error,
                                    "The shader compiler could not create, read, or clean its isolated scratch files.",
                                    "Check the configured scratch root permissions and available storage, then retry the bounded cook.");
    const ErrorCodeDescriptor ToolProcessFailed = Detail::
        MakeErrorDescriptor(Domain, "render.shader_compiler.tool_process_failed", ErrorSeverity::Error,
                            "A locked shader compiler, translator, or validator process failed.",
                            "Inspect the bounded source diagnostic and exact tool identity, then correct the source or host toolchain.");
    const ErrorCodeDescriptor ToolOutputInvalid =
        Detail::MakeErrorDescriptor(Domain, "render.shader_compiler.tool_output_invalid", ErrorSeverity::Error,
                                    "A shader tool emitted an absent, malformed, or over-budget artifact.",
                                    "Inspect the exact tool invocation and reject the candidate generation until the artifact validates.");
}  // namespace Horo::Render::ShaderCompilerPipelineErrors
