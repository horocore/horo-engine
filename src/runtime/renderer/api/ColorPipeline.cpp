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
            return true;
        }

        /** @brief Admits only the actual analytic program's named SDR100 sRGB output route. */
        bool OutputRouteValid(const ColorPipelineRequest &r) noexcept {
            const auto &c = r.output.active;
            return c.dynamicRange == RenderDisplayDynamicRange::Standard && c.colorSpace == RenderDisplayColorSpace::Srgb &&
                   c.transfer == RenderDisplayTransferFunction::Srgb;
        }

        /** @brief Checks the distinct analytic output identity and an optional same-configuration AP1 creative look. */
        bool TransformCompatible(const std::shared_ptr<const CookedColorOutputProgram> &analytic, ColorLookMode mode,
                                 const std::shared_ptr<const CookedColorTransform> &look) noexcept {
            if (!analytic || static_cast<unsigned>(mode) > static_cast<unsigned>(ColorLookMode::Enabled))
                return false;
            const auto &d = analytic->Descriptor();
            if (d.profile != ColorOutputProfile::Aces2Sdr100Rec709LinearV1)
                return false;
            if (!look)
                return mode == ColorLookMode::Disabled;
            const auto &asset = look->Descriptor();
            return asset.role == ColorTransformRole::CreativeLook && asset.configurationRelease == d.configurationRelease &&
                   asset.configurationDigest == d.configurationDigest;
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

        /** @brief Checks actual frame identity and output admission without changing active state. */
        Result<void> ValidateRequest(const ColorPipelineRequest &r) {
            if (r.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ColorPipelineErrors::Cancelled));
            if (!SceneShapeValid(r.sceneColor) || !SceneIdentityValid(r))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::IncompatibleInput));
            if (!OutputValid(r) || !OutputRouteValid(r))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::UnsupportedOutput));
            if (static_cast<unsigned>(r.dither) > static_cast<unsigned>(ColorDitherPolicy::Ordered))
                return Result<void>::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
            return Result<void>::Success();
        }

        /** @brief Reuses canonical exposure settings/state validation independently of cold transform selection. */
        Result<void> ValidateExposure(const ColorPipelineRequest &r) {
            if (auto settings = ValidateExposureSettings(r.exposureSettings); settings.HasError())
                return settings;
            if (auto state = ValidateExposureState(r.exposure, r.compatibility, r.settingsGeneration, r.submission); state.HasError())
                return state;
            if (!ExposureModeCompatible(r))
                return Result<void>::Failure(MakeError(ExposureErrors::IncompatibleState));
            return Result<void>::Success();
        }

        /** @brief Reuses canonical numeric ranges before explicitly rejecting unsupported nondefault grading semantics. */
        Result<void> ValidateGrading(const ColorGradingSettings &grading) {
            PostProcessSettings settings;
            settings.colorGrading = grading;
            if (auto valid = ValidatePostProcessSettings(settings); valid.HasError())
                return valid;
            if (grading != ColorGradingSettings{})
                return Result<void>::Failure(MakeError(ColorPipelineErrors::UnsupportedGrading));
            return Result<void>::Success();
        }

        /** @brief Accounts for analytic source/original/upload tables plus optional original/atlas look bytes. */
        bool WithinBudget(const CookedColorOutputProgram &analytic, const std::shared_ptr<const CookedColorTransform> &look,
                          std::uint64_t maximum) noexcept {
            if (maximum == 0 || maximum > 64U * 1024U * 1024U)
                return false;
            auto bytes = analytic.PayloadBytes();
            if (look)
                bytes += look->PayloadBytes();
            return bytes <= maximum;
        }
    }  // namespace

    /** @copydoc ColorTransformSelection::ColorTransformSelection */
    ColorTransformSelection::ColorTransformSelection(std::shared_ptr<const CookedColorOutputProgram> analytic, ColorLookMode mode,
                                                     std::shared_ptr<const CookedColorTransform> look) noexcept
        : analytic_(std::move(analytic)), look_(std::move(look)), mode_(mode) {}

    /** @copydoc ColorTransformSelection::Analytic */
    const std::shared_ptr<const CookedColorOutputProgram> &ColorTransformSelection::Analytic() const noexcept {
        return analytic_;
    }

    /** @copydoc ColorTransformSelection::Look */
    const std::shared_ptr<const CookedColorTransform> &ColorTransformSelection::Look() const noexcept {
        return look_;
    }

    /** @copydoc ColorTransformSelection::Mode */
    ColorLookMode ColorTransformSelection::Mode() const noexcept {
        return mode_;
    }

    /** @copydoc PrepareColorTransformSelection */
    Result<ColorTransformSelection> PrepareColorTransformSelection(const std::shared_ptr<const CookedColorOutputProgram> &analytic,
                                                                   ColorLookMode mode,
                                                                   const std::shared_ptr<const CookedColorTransform> &look,
                                                                   const ColorGradingSettings &grading, std::uint64_t maximumBytes) {
        using Return = Result<ColorTransformSelection>;
        if (!TransformCompatible(analytic, mode, look))
            return Return::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        if (auto valid = ValidateGrading(grading); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (!WithinBudget(*analytic, look, maximumBytes))
            return Return::Failure(MakeError(ColorPipelineErrors::CapacityExceeded));
        return Return::Success(ColorTransformSelection{analytic, mode, look});
    }

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
        if (auto valid = ValidateExposure(request); valid.HasError())
            return Result<ColorPipelinePlan>::Failure(valid.ErrorValue());
        if (auto selection = PrepareColorTransformSelection(request.outputTransform, request.lookMode, request.look, request.grading,
                                                            request.maximumTransformBytes);
            selection.HasError())
            return Result<ColorPipelinePlan>::Failure(selection.ErrorValue());
        if (request.cancellation.IsCancellationRequested())
            return Result<ColorPipelinePlan>::Failure(MakeError(ColorPipelineErrors::Cancelled));
        return Result<ColorPipelinePlan>::Success(ColorPipelinePlan{request});
    }

    namespace ColorPipelineErrors {
        namespace {
            const ErrorDomainId Domain{"render.color_pipeline"};
        }

        const ErrorCodeDescriptor UnsupportedGrading =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.grading_unsupported", ErrorSeverity::Error,
                                        "Numeric creative grading is unsupported by this output route.",
                                        "Use default numeric controls and an explicitly cooked creative look until numeric grading "
                                        "semantics are implemented.");

        const ErrorCodeDescriptor InvalidTransform =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.transform_invalid", ErrorSeverity::Error,
                                        "Cooked color transform metadata or payload is invalid.",
                                        "Supply the exact pinned analytic output program/tables and separately declared finite "
                                        "creative-look data.");
        const ErrorCodeDescriptor IncompatibleInput =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.input_incompatible", ErrorSeverity::Error,
                                        "Scene color identity is incompatible.",
                                        "Supply unexposed ACEScg RGBA16F and the same complete view/history/plan generations.");
        const ErrorCodeDescriptor UnsupportedOutput =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.output_unsupported", ErrorSeverity::Error,
                                        "Requested output is unsupported by this color plan.",
                                        "Supply exact named SDR100 sRGB output admission and actual display facts; other profiles have no "
                                        "implicit fallback.");
        const ErrorCodeDescriptor CapacityExceeded =
            Detail::MakeErrorDescriptor(Domain, "render.color_pipeline.capacity_exceeded", ErrorSeverity::Error,
                                        "Color transform storage exceeds the declared bound.",
                                        "Reserve the complete analytic source/tables and optional look within the 64 MiB hard limit.");
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
