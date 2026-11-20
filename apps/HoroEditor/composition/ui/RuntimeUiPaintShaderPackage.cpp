#include "RuntimeUiPaintShaderPackage.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Preparation = Result<std::shared_ptr<const Render::CookedUiPaintKernel>>;

        struct UiPaintPackageProfile final {
            Render::ShaderTargetRequirement target;
            Render::RenderTextureFormat color;
            Render::RenderTextureFormat stencil;
        };

        /** @brief Admits one captured backend profile with immutable package bytes and strict pinned compiler identity. */
        Preparation DecodePackage(const std::span<const std::uint8_t> payload, const std::span<const std::uint8_t> metadata,
                                  const Render::ShaderTargetBackend selectedBackend, const UiPaintPackageProfile &profile) {
            const auto &target = profile.target;
            if (selectedBackend != target.backend)
                return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::UnsupportedTarget));
            if (payload.empty() || payload.size() > 16U * 1024U * 1024U || metadata.empty() || metadata.size() > 1024U * 1024U)
                return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ToolOutputInvalid));
            try {
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
                auto kernel = std::make_shared<Render::CookedUiPaintKernel>(std::move(manifest), target, std::move(artifact), profile.color,
                                                                            profile.stencil);
                if (const auto valid = Render::ValidateCookedUiPaintKernel(*kernel, target.backend); valid.HasError())
                    return Preparation::Failure(valid.ErrorValue());
                return Preparation::Success(std::move(kernel));
            } catch (const std::bad_alloc &) {
                return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
            }
        }
    }  // namespace

    /** @copydoc ProjectUiPaintMetalTarget */
    Render::ShaderTargetRequirement ProjectUiPaintMetalTarget() {
        return {Render::ShaderTargetBackend::Metal, Render::ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
    }

    /** @copydoc DecodeProjectUiPaintMetalPackage */
    Preparation DecodeProjectUiPaintMetalPackage(const std::span<const std::uint8_t> payload, const std::span<const std::uint8_t> metadata,
                                                 const Render::ShaderTargetBackend selectedBackend) {
        return DecodePackage(payload, metadata, selectedBackend,
                             {ProjectUiPaintMetalTarget(), Render::RenderTextureFormat::Bgra8Unorm,
                              Render::RenderTextureFormat::Depth32FloatStencil8});
    }

    /** @copydoc DecodeProjectUiPaintVulkanScenePackage */
    Preparation DecodeProjectUiPaintVulkanScenePackage(const std::span<const std::uint8_t> payload,
                                                       const std::span<const std::uint8_t> metadata,
                                                       const Render::ShaderTargetBackend selectedBackend) {
        return DecodePackage(payload, metadata, selectedBackend,
                             {Render::UiPaintVulkanTarget(), Render::RenderTextureFormat::Rgba16Float,
                              Render::RenderTextureFormat::Depth32FloatStencil8});
    }
}  // namespace Horo::Application::Internal
