#include "Horo/Runtime/Render/ReconstructionFrame.h"

#include "Horo/Runtime/Render/TemporalHistoryErrors.h"
#include "RenderErrorDescriptor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace Horo::Render::ReconstructionErrors {
    namespace {
        const ErrorDomainId Domain{"render.reconstruction"};
    }

    const ErrorCodeDescriptor InvalidFrame =
        Detail::MakeErrorDescriptor(Domain, "render.reconstruction.frame_invalid", ErrorSeverity::Error,
                                    "The reconstruction frame metadata is invalid.",
                                    "Use a current device snapshot, complete history compatibility and ordered finite real-frame inputs.");
    const ErrorCodeDescriptor MissingGuides =
        Detail::MakeErrorDescriptor(Domain, "render.reconstruction.guides_missing", ErrorSeverity::Error,
                                    "Temporal reconstruction requires current linear depth and motion.",
                                    "Supply both guides at the actual render extent.");
    const ErrorCodeDescriptor IncompatibleInput =
        Detail::MakeErrorDescriptor(Domain, "render.reconstruction.input_incompatible", ErrorSeverity::Error,
                                    "A reconstruction input is incompatible with this real frame.",
                                    "Verify semantic format, current frame and resource owner, mask producer and exposure scope.");
    const ErrorCodeDescriptor IncompatibleOutput =
        Detail::MakeErrorDescriptor(Domain, "render.reconstruction.output_incompatible", ErrorSeverity::Error,
                                    "The reconstruction output cannot be written as declared.",
                                    "Use a distinct target-extent linear HDR image with compatible graph usage and lifetime.");
    const ErrorCodeDescriptor UnsupportedResource =
        Detail::MakeErrorDescriptor(Domain, "render.reconstruction.resource_unsupported", ErrorSeverity::Error,
                                    "The selected device cannot support a reconstruction texture descriptor.",
                                    "Select a declared supported reconstruction recipe before preparing the frame.");
}  // namespace Horo::Render::ReconstructionErrors

namespace Horo::Render {
    namespace {
        using enum RenderTextureFormat;
        constexpr std::array ColorFormats{Rgba16Float, Rgba32Float};
        constexpr std::array DepthFormats{R16Float, R32Float};
        constexpr std::array MotionFormats{Rg16Float, Rg32Float};
        constexpr std::array MaskFormats{R8Unorm, R16Float, R32Float};

        /** @brief Checks the bounded single-view image shape for one semantic role. */
        bool MatchesImage(const RenderTextureDescriptor &image, FramebufferExtent extent, std::span<const RenderTextureFormat> formats,
                          RenderTextureUsage usage) noexcept {
            return image.IsValid() && image.dimension == RenderTextureDimension::TwoD && image.extent == extent && image.mipCount == 1 &&
                   image.layerCount == 1 && image.sampleCount == 1 && HasTextureUsage(image.usage, usage) &&
                   std::ranges::find(formats, image.format) != formats.end();
        }

        /** @brief Spatial shares view, extent and device facts without requiring a temporal history generation. */
        bool MatchesView(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities) noexcept {
            const auto &view = request.compatibility;
            if (!capabilities.IsValid() || view.deviceGeneration != capabilities.deviceIncarnation)
                return false;
            if (request.kind == ReconstructionKind::Temporal)
                return view.IsValid();
            return view.view.IsValid() && view.renderExtent.IsValid() && view.targetExtent.IsValid();
        }

        /** @brief Admits only the selected device and exact real-frame timing and projection. */
        bool MatchesFrame(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities) noexcept {
            const auto &time = request.timing;
            return request.kind <= ReconstructionKind::Temporal && request.colorStage <= ReconstructionColorStage::PreExposed &&
                   request.resourceOwner.IsValid() && MatchesView(request, capabilities) && request.projection.IsValid() &&
                   time.frameId != 0 && time.predecessorFrameId < time.frameId && time.realFrameDelta.count() >= 0 &&
                   Math::IsFinite(request.currentJitterUv) && Math::IsFinite(request.previousJitterUv) &&
                   request.motionConvention == RenderMotionVectorConvention::PreviousUvMinusCurrentUv;
        }

        /** @brief Validates scale conversion or the exact GPU exposure owner without reading device data. */
        bool MatchesExposure(const ReconstructionFrameRequest &request) noexcept {
            if (const auto *scales = std::get_if<ReconstructionExposureScales>(&request.exposure)) {
                if (!std::isfinite(scales->current) || !std::isfinite(scales->previous) || scales->current <= 0.0F ||
                    scales->previous <= 0.0F)
                    return false;
                const float ratio = scales->current / scales->previous;
                return std::isfinite(ratio) && ratio > 0.0F;
            }
            const auto *buffers = std::get_if<ReconstructionExposureBuffers>(&request.exposure);
            if (!buffers || buffers->view != request.compatibility.view || buffers->generation == 0 ||
                buffers->generation != request.compatibility.exposureGeneration || !buffers->current.IsValid() ||
                buffers->current.owner != request.resourceOwner)
                return false;
            const bool needsPrevious =
                request.kind == ReconstructionKind::Temporal && request.timing.predecessorFrameId != 0 && !request.reset;
            return buffers->previous ? buffers->previous->IsValid() && buffers->previous->owner == request.resourceOwner : !needsPrevious;
        }

        /** @brief Bounded set of sample inputs used to reject slot aliases. All members have the same resource owner. */
        struct ReadTextures final {
            std::array<RenderTextureHandle, 7> images;
            std::size_t count{};

            [[nodiscard]] bool UsesSlot(RenderTextureHandle handle) const noexcept {
                return std::ranges::any_of(std::span{images}.first(count), [handle](const auto prior) {
                    return prior.slot == handle.slot;
                });
            }
        };

        /** @brief Admits one typed render-extent texture against the real frame and selected device. */
        Result<void> AdmitRead(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities,
                               const ReconstructionTextureInput &input, std::span<const RenderTextureFormat> formats, ReadTextures &reads) {
            if (!input.texture.IsValid() || input.texture.owner != request.resourceOwner || input.view != request.compatibility.view ||
                input.frameId != request.timing.frameId ||
                !MatchesImage(input.descriptor, request.compatibility.renderExtent, formats, RenderTextureUsage::Sampled) ||
                reads.UsesSlot(input.texture))
                return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleInput));
            if (!capabilities.Supports(input.descriptor))
                return Result<void>::Failure(MakeError(ReconstructionErrors::UnsupportedResource));
            reads.images[reads.count++] = input.texture;
            return Result<void>::Success();
        }

        /** @brief Reads the color and whichever typed guide and mask roles the caller supplied. */
        Result<void> AdmitReads(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities,
                                ReadTextures &reads) {
            if (auto result = AdmitRead(request, capabilities, request.color, ColorFormats, reads); result.HasError())
                return result;
            if (request.positiveLinearDepth)
                if (auto result = AdmitRead(request, capabilities, *request.positiveLinearDepth, DepthFormats, reads); result.HasError())
                    return result;
            if (request.motion)
                if (auto result = AdmitRead(request, capabilities, *request.motion, MotionFormats, reads); result.HasError())
                    return result;
            const auto &masks = request.masks;
            for (const auto *mask : {&masks.reactive, &masks.transparency, &masks.disocclusion, &masks.composition}) {
                if (!*mask)
                    continue;
                if (!(*mask)->producer.IsValid())
                    return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleInput));
                if (auto result = AdmitRead(request, capabilities, (*mask)->image, MaskFormats, reads); result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        /** @brief Admits a target-extent HDR write declared through the canonical graph lifetime and usage types. */
        Result<void> AdmitOutput(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities,
                                 const ReadTextures &reads) {
            const auto &output = request.output;
            const bool transient = output.lifetime == RenderGraphResourceClass::Transient;
            const bool validResident = output.resident && output.resident->IsValid() && output.resident->owner == request.resourceOwner &&
                                       !reads.UsesSlot(*output.resident);
            if (output.lifetime > RenderGraphResourceClass::History || (transient ? output.resident.has_value() : !validResident))
                return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleOutput));
            if (output.writeUsage != RenderGraphUsageKind::Storage && output.writeUsage != RenderGraphUsageKind::ColorAttachment)
                return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleOutput));
            const auto usage =
                output.writeUsage == RenderGraphUsageKind::Storage ? RenderTextureUsage::Storage : RenderTextureUsage::RenderAttachment;
            if (!MatchesImage(output.descriptor, request.compatibility.targetExtent, ColorFormats, usage))
                return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleOutput));
            if (!capabilities.Supports(output.descriptor))
                return Result<void>::Failure(MakeError(ReconstructionErrors::UnsupportedResource));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateReconstructionFrame */
    Result<void> ValidateReconstructionFrame(const ReconstructionFrameRequest &request, const RenderCapabilitySnapshot &capabilities) {
        if (!MatchesFrame(request, capabilities) || (request.kind == ReconstructionKind::Spatial && request.reset))
            return Result<void>::Failure(MakeError(ReconstructionErrors::InvalidFrame));
        if (request.reset && !IsKnownTemporalHistoryResetCause(*request.reset))
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidResetCause));
        if (request.kind == ReconstructionKind::Temporal && (!request.positiveLinearDepth || !request.motion))
            return Result<void>::Failure(MakeError(ReconstructionErrors::MissingGuides));
        if (!MatchesExposure(request))
            return Result<void>::Failure(MakeError(ReconstructionErrors::IncompatibleInput));
        ReadTextures reads{};
        if (auto result = AdmitReads(request, capabilities, reads); result.HasError())
            return result;
        return AdmitOutput(request, capabilities, reads);
    }

    /** @copydoc BeginTemporalReconstructionFrame */
    Result<TemporalHistoryFrame> BeginTemporalReconstructionFrame(const ReconstructionFrameRequest &request,
                                                                  const RenderCapabilitySnapshot &capabilities, TemporalHistoryStore &store,
                                                                  TemporalHistoryHandle history) {
        if (request.kind != ReconstructionKind::Temporal)
            return Result<TemporalHistoryFrame>::Failure(MakeError(ReconstructionErrors::InvalidFrame));
        if (auto result = ValidateReconstructionFrame(request, capabilities); result.HasError())
            return Result<TemporalHistoryFrame>::Failure(result.ErrorValue());
        return store.BeginFrame(TemporalHistoryFrameRequest{history, request.compatibility, request.resourceOwner, request.timing.frameId,
                                                            request.timing.predecessorFrameId, request.reset});
    }
}  // namespace Horo::Render
