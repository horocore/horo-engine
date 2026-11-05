#include "Horo/Runtime/Render/ColorPipeline.h"

#include "RenderErrorDescriptor.h"

#include <cmath>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Checks sampled single-view canonical scene storage. */
        bool SceneShapeValid(const PostProcessTextureInput &s) noexcept {
            const auto &d = s.descriptor;
            return s.texture.IsValid() && d.IsValid() && d.dimension == RenderTextureDimension::TwoD &&
                   d.format == RenderTextureFormat::Rgba16Float && d.layerCount == 1 && d.mipCount == 1 && d.sampleCount == 1 &&
                   HasTextureUsage(d.usage, RenderTextureUsage::Sampled);
        }

        /** @brief Checks complete scene and color-plan compatibility metadata. */
        bool SceneIdentityValid(const ColorPipelineRequest &r) noexcept {
            return r.generation > 0 && r.settingsGeneration > 0 && r.compatibility.IsValid() &&
                   r.generation == r.compatibility.colorGeneration && r.sceneColor.compatibility == r.compatibility &&
                   r.sceneColor.descriptor.extent == r.compatibility.renderExtent &&
                   r.sceneColor.semantic == PostProcessSemantic::SceneColor &&
                   r.sceneColor.representation == PostProcessRepresentation::UnexposedAcesCg;
        }

        /** @brief Checks declared quantization against admitted output storage. */
        bool BitDepthValid(const ColorPipelineRequest &r) noexcept {
            const auto format = r.output.active.format;
            if (format == RenderTextureFormat::Rgba16Float || format == RenderTextureFormat::Rgba32Float)
                return r.outputBitDepth == 10 || r.outputBitDepth == 16;
            return r.outputBitDepth == 8;
        }

        /** @brief Checks exact named native output metadata and current display facts. */
        bool OutputValid(const ColorPipelineRequest &r) noexcept {
            if (!r.output.active.IsValid() || r.output.requested.color != r.output.active || r.output.revision == 0 ||
                !r.output.display.IsValid() || !r.output.display.attached)
                return false;
            if (r.output.active.matching != RenderOutputColorMatching::ColorSpace)
                return false;
            if (!BitDepthValid(r))
                return false;
            if (r.output.active.dynamicRange == RenderDisplayDynamicRange::High)
                return r.outputBitDepth >= 10 && r.output.display.hdr == RenderDisplayHdrSupport::Supported;
            return true;
        }

        /** @brief Admits only the separately named SDR sRGB and HDR10 PQ routes. */
        bool OutputRouteValid(const ColorPipelineRequest &r) noexcept {
            const auto &c = r.output.active;
            if (c.dynamicRange == RenderDisplayDynamicRange::Standard)
                return c.colorSpace == RenderDisplayColorSpace::Srgb && c.transfer == RenderDisplayTransferFunction::Srgb;
            return c.colorSpace == RenderDisplayColorSpace::Rec2020 && c.transfer == RenderDisplayTransferFunction::Pq;
        }

        /** @brief Checks explicit output role and common look configuration identity. */
        bool TransformCompatible(const ColorPipelineRequest &r) noexcept {
            if (!r.outputTransform)
                return false;
            const auto &d = r.outputTransform->Descriptor();
            if (d.role != r.requiredOutput || d.output != r.output.active)
                return false;
            if (r.requiredOutput != ColorTransformRole::Aces2Output && r.requiredOutput != ColorTransformRole::NeutralDiagnostic)
                return false;
            if (!r.look)
                return true;
            const auto &look = r.look->Descriptor();
            return look.role == ColorTransformRole::CreativeLook && look.configurationRelease == d.configurationRelease &&
                   look.configurationDigest == d.configurationDigest;
        }

        /** @brief Checks manual or initial state values against the authored exposure selection. */
        bool ExposureModeCompatible(const ColorPipelineRequest &r) noexcept {
            const auto &s = r.exposure;
            if (r.exposureSettings.mode == ExposureMode::Manual)
                return s.mode == ExposureMode::Manual && s.diagnosticEv == r.exposureSettings.manualEv + r.exposureSettings.compensationEv;
            if (s.mode == ExposureMode::AutomaticHistogram)
                return true;
            return s.reason == ExposureSampleReason::Initial &&
                   s.diagnosticEv == r.exposureSettings.initialEv + r.exposureSettings.compensationEv;
        }

        /** @brief Checks cold identity and output admission without changing active state. */
        Result<void> ValidateRequest(const ColorPipelineRequest &r) {
            if (r.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ColorPipelineErrors::Cancelled));
            if (!SceneShapeValid(r.sceneColor) || !SceneIdentityValid(r))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::IncompatibleInput));
            if (!OutputValid(r) || !OutputRouteValid(r))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::UnsupportedOutput));
            if (!TransformCompatible(r) || static_cast<unsigned>(r.dither) > static_cast<unsigned>(ColorDitherPolicy::Ordered))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
            return Result<void>::Success();
        }

        /** @brief Reuses canonical exposure and grading validation. */
        Result<void> ValidateExposureAndGrading(const ColorPipelineRequest &r) {
            if (auto settings = ValidateExposureSettings(r.exposureSettings); settings.HasError())
                return settings;
            if (auto state = ValidateExposureState(r.exposure, r.compatibility, r.settingsGeneration, r.submission); state.HasError())
                return state;
            if (!ExposureModeCompatible(r))
                return Result<void>::Failure(MakeError(ExposureErrors::IncompatibleState));
            PostProcessSettings settings;
            settings.colorGrading = r.grading;
            return ValidatePostProcessSettings(settings);
        }

        /** @brief Checks complete pinned LUT bytes against the declared hard ceiling. */
        bool WithinBudget(const ColorPipelineRequest &r) noexcept {
            if (r.maximumTransformBytes == 0 || r.maximumTransformBytes > 64U * 1024U * 1024U)
                return false;
            auto bytes = static_cast<std::uint64_t>(r.outputTransform->Rgb().size()) * sizeof(float);
            if (r.look)
                bytes += static_cast<std::uint64_t>(r.look->Rgb().size()) * sizeof(float);
            return bytes <= r.maximumTransformBytes;
        }
    }  // namespace

    /** @copydoc ColorPipelinePlan::ColorPipelinePlan */
    ColorPipelinePlan::ColorPipelinePlan(ColorPipelineRequest request) : request_(std::move(request)) {}

    /** @copydoc ColorPipelinePlan::Inputs */
    const ColorPipelineRequest &ColorPipelinePlan::Inputs() const noexcept {
        return request_;
    }

    /** @copydoc PrepareColorPipeline */
    Result<ColorPipelinePlan> PrepareColorPipeline(const ColorPipelineRequest &request) {
        if (auto valid = ValidateRequest(request); valid.HasError())
            return Result<ColorPipelinePlan>::Failure(valid.ErrorValue());
        if (auto valid = ValidateExposureAndGrading(request); valid.HasError())
            return Result<ColorPipelinePlan>::Failure(valid.ErrorValue());
        if (!WithinBudget(request))
            return Result<ColorPipelinePlan>::Failure(MakeError(ColorPipelineErrors::CapacityExceeded));
        if (request.cancellation.IsCancellationRequested())
            return Result<ColorPipelinePlan>::Failure(MakeError(ColorPipelineErrors::Cancelled));
        return Result<ColorPipelinePlan>::Success(ColorPipelinePlan{request});
    }

    namespace ColorPipelineErrors {
        namespace {
            const ErrorDomainId Domain{"render.color_pipeline"};
        }

        const ErrorCodeDescriptor InvalidTransform =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.transform_invalid", ErrorSeverity::Error,
                                        "Cooked color transform metadata or payload is invalid.",
                                        "Supply exact versioned finite RGB32F data, domain, pinned digests and explicit transform role.");
        const ErrorCodeDescriptor IncompatibleInput =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.input_incompatible", ErrorSeverity::Error,
                                        "Scene color identity is incompatible.",
                                        "Supply unexposed ACEScg RGBA16F and the same complete view/history/plan generations.");
        const ErrorCodeDescriptor UnsupportedOutput =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.output_unsupported", ErrorSeverity::Error,
                                        "Requested output is unsupported by this color plan.",
                                        "Supply exact named SDR sRGB or HDR10 PQ output admission and actual display facts; no implicit "
                                        "fallback is selected.");
        const ErrorCodeDescriptor CapacityExceeded =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.capacity_exceeded", ErrorSeverity::Error,
                                        "Color transform storage exceeds the declared bound.",
                                        "Reserve the complete look and output LUTs within the 64 MiB hard limit.");
        const ErrorCodeDescriptor Cancelled =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.cancelled", ErrorSeverity::Error,
                                        "Color plan preparation was cancelled.",
                                        "Keep the active compatible plan and prepare a new candidate at the next permitted boundary.");
        const ErrorCodeDescriptor AllocationFailed =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.allocation_failed", ErrorSeverity::Error,
                                        "Color transform storage allocation failed.",
                                        "Reduce the bounded cooked payload and retry cold preparation.");
    }  // namespace ColorPipelineErrors
}  // namespace Horo::Render
