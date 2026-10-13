#include "Horo/Runtime/Render/RenderShaderResourceErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::RenderShaderResourceErrors {
    namespace {
        const ErrorDomainId Domain{"render.shader_resource"};
    }

    const ErrorCodeDescriptor InvalidModule =
        Detail::MakeErrorDescriptor(Domain, "render.shader_resource.invalid_module", ErrorSeverity::Error,
                                    "Cooked shader module identity is invalid.",
                                    "Supply the exact cooked stage, target and artifact/interface identities.");
    const ErrorCodeDescriptor InvalidPipeline =
        Detail::MakeErrorDescriptor(Domain, "render.shader_resource.invalid_pipeline", ErrorSeverity::Error,
                                    "Graphics pipeline identity is invalid.",
                                    "Use matching resident stages, validated reflection and canonical fixed-state identity.");
    const ErrorCodeDescriptor Unsupported =
        Detail::MakeErrorDescriptor(Domain, "render.shader_resource.unsupported", ErrorSeverity::Error,
                                    "The selected backend cannot realize the requested shader resource.",
                                    "Admit an explicitly supported cooked target without backend substitution.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.shader_resource.allocation_failed", ErrorSeverity::Error,
                                    "Shader preparation metadata allocation failed.",
                                    "Reduce the bounded preparation envelope and retry during loading.");
    const ErrorCodeDescriptor WrongThread =
        Detail::MakeErrorDescriptor(Domain, "render.shader_resource.wrong_thread", ErrorSeverity::Error,
                                    "Shader resource preparation requires the render owner thread.",
                                    "Dispatch preparation and lease release to the frontend's creating thread.");
}  // namespace Horo::Render::RenderShaderResourceErrors
