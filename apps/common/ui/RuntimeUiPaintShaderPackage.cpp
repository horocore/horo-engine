#include "RuntimeUiPaintShaderPackage.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <new>

namespace Horo::Application::Internal {
    /** @copydoc ProjectUiPaintMetalTarget */
    Render::ShaderTargetRequirement ProjectUiPaintMetalTarget() {
        return {Render::ShaderTargetBackend::Metal, Render::ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
    }

    /** @copydoc DecodeProjectUiPaintMetalPackage */
    Result<std::shared_ptr<const Render::CookedUiPaintKernel>> DecodeProjectUiPaintMetalPackage(
        const std::span<const std::uint8_t> payload, const std::span<const std::uint8_t> metadata,
        const Render::ShaderTargetBackend selectedBackend) {
        using Preparation = Result<std::shared_ptr<const Render::CookedUiPaintKernel>>;
        if (selectedBackend != Render::ShaderTargetBackend::Metal)
            return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::UnsupportedTarget));
        if (payload.empty() || payload.size() > 16U * 1024U * 1024U || metadata.empty() || metadata.size() > 1024U * 1024U)
            return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolOutputInvalid));
        try {
            const auto target = ProjectUiPaintMetalTarget();
            auto manifest = Render::MakeUiPaintShaderManifest(target);
            auto decoded = Render::DecodeReflectedShaderArtifact(manifest, target, payload, metadata);
            if (decoded.HasError())
                return Preparation::Failure(decoded.ErrorValue());
            auto record = std::make_shared<const Render::ReflectedShaderArtifact>(std::move(decoded).Value());
            Render::CompiledShaderArtifact artifact{.backend = target.backend,
                                                    .payloadFormat = target.payloadFormat,
                                                    .artifactKey = record->compiledArtifactKey,
                                                    .payload = {payload.begin(), payload.end()},
                                                    .reflection = std::move(record)};
            auto kernel = std::make_shared<Render::CookedUiPaintKernel>(
                Render::CookedUiPaintKernel{std::move(manifest), target, std::move(artifact), Render::RenderTextureFormat::Bgra8Unorm,
                                            Render::RenderTextureFormat::Depth32FloatStencil8});
            if (const auto valid = Render::ValidateCookedUiPaintKernel(*kernel, target.backend); valid.HasError())
                return Preparation::Failure(valid.ErrorValue());
            return Preparation::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Application::Internal
