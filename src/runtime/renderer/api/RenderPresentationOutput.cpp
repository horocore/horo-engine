#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#include "Horo/Runtime/Render/RenderBackend.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Horo::Render {
    /** @copydoc IRenderBackend::PollPresentationAvailability */
    Result<bool> IRenderBackend::PollPresentationAvailability(FramebufferExtent) {
        return Result<bool>::Success(true);
    }

    /** @copydoc IRenderBackend::ConfigurePresentationOutput */
    Result<RenderPresentationOutput> IRenderBackend::ConfigurePresentationOutput(const RenderPresentationOutputRequest &) {
        return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
    }

    /** @copydoc IRenderBackend::PresentationOutput */
    Result<RenderPresentationOutput> IRenderBackend::PresentationOutput() const {
        return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
    }

    /** @copydoc RenderOutputColor::IsValid */
    bool RenderOutputColor::IsValid() const noexcept {
        using enum RenderTextureFormat;
        if (const bool colorFormat = format == Rgba8Unorm || format == Rgba8UnormSrgb || format == Bgra8Unorm || format == Bgra8UnormSrgb ||
                                     format == Rgba16Float || format == Rgba32Float;
            !colorFormat || static_cast<std::uint8_t>(colorSpace) > static_cast<std::uint8_t>(RenderDisplayColorSpace::Rec2020) ||
            static_cast<std::uint8_t>(transfer) > static_cast<std::uint8_t>(RenderDisplayTransferFunction::Hlg) ||
            static_cast<std::uint8_t>(dynamicRange) > static_cast<std::uint8_t>(RenderDisplayDynamicRange::High))
            return false;
        if (static_cast<std::uint8_t>(matching) > static_cast<std::uint8_t>(RenderOutputColorMatching::ColorSpace))
            return false;
        if (matching == RenderOutputColorMatching::Unmanaged)
            return format == Bgra8Unorm && colorSpace == RenderDisplayColorSpace::Srgb && transfer == RenderDisplayTransferFunction::Srgb &&
                   dynamicRange == RenderDisplayDynamicRange::Standard;
        if (dynamicRange == RenderDisplayDynamicRange::Standard)
            return transfer == RenderDisplayTransferFunction::Srgb || transfer == RenderDisplayTransferFunction::Linear;
        return (format == Rgba16Float || format == Rgba32Float) &&
               ((transfer == RenderDisplayTransferFunction::Linear &&
                 (colorSpace == RenderDisplayColorSpace::Srgb || colorSpace == RenderDisplayColorSpace::DisplayP3)) ||
                (colorSpace == RenderDisplayColorSpace::Rec2020 && transfer == RenderDisplayTransferFunction::Pq));
    }

    /** @copydoc RenderPresentationOutputRequest::IsValid */
    bool RenderPresentationOutputRequest::IsValid() const noexcept {
        return color.IsValid();
    }

    /** @copydoc RenderOutputDisplayFacts::IsValid */
    bool RenderOutputDisplayFacts::IsValid() const noexcept {
        return revision != 0 && static_cast<std::uint8_t>(hdr) <= static_cast<std::uint8_t>(RenderDisplayHdrSupport::Supported) &&
               (!linearHeadroom.has_value() || (std::isfinite(*linearHeadroom) && *linearHeadroom >= 1.0F)) &&
               (!potentialLinearHeadroom.has_value() || (std::isfinite(*potentialLinearHeadroom) && *potentialLinearHeadroom >= 1.0F)) &&
               (!linearHeadroom.has_value() || !potentialLinearHeadroom.has_value() || *linearHeadroom <= *potentialLinearHeadroom);
    }

    /** @copydoc RenderOutputCapabilities::IsValid */
    bool RenderOutputCapabilities::IsValid() const noexcept {
        if (count == 0 || count > colors.size())
            return false;
        const std::span admitted{colors.data(), count};
        for (std::size_t index = 0; index < admitted.size(); ++index) {
            if (!admitted[index].IsValid() ||
                std::find(admitted.begin(), admitted.begin() + index, admitted[index]) != admitted.begin() + index)
                return false;
        }
        return true;
    }

    /** @copydoc NegotiatePresentationOutput */
    Result<RenderPresentationOutput> NegotiatePresentationOutput(const RenderPresentationOutputRequest &request,
                                                                 const RenderOutputCapabilities &capabilities,
                                                                 const RenderOutputDisplayFacts &display, const std::uint64_t revision) {
        if (!request.IsValid() || !capabilities.IsValid() || !display.IsValid() || revision == 0)
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::InvalidOutput));
        if (!display.attached)
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
        if (const std::span supported{capabilities.colors.data(), capabilities.count};
            std::ranges::find(supported, request.color) == supported.end())
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
        if (request.color.dynamicRange == RenderDisplayDynamicRange::High &&
            (display.hdr != RenderDisplayHdrSupport::Supported ||
             (request.color.transfer == RenderDisplayTransferFunction::Linear &&
              (!display.potentialLinearHeadroom.has_value() || *display.potentialLinearHeadroom <= 1.0F))))
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::HdrUnavailable));
        return Result<RenderPresentationOutput>::Success({request, request.color, display, revision});
    }
}  // namespace Horo::Render
