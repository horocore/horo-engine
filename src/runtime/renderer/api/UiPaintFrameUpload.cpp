#include "Horo/Runtime/Render/UiPaintFrameUpload.h"

#include "Horo/Runtime/Render/RenderBackend.h"
#include "UiPaintParameterValidation.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    namespace UiPaintFrameErrors {
        const ErrorCodeDescriptor InvalidInput{ErrorDomainId{"horo.render"}, ErrorCode{"render.ui_paint.frame.invalid"},
                                               ErrorSeverity::Error, "The complete UI frame payload or native slot is invalid.",
                                               "Supply bounded finite geometry and exact compatible residents with a newer revision."};
        const ErrorCodeDescriptor Pending{ErrorDomainId{"horo.render"}, ErrorCode{"render.ui_paint.frame.pending"}, ErrorSeverity::Info,
                                          "The UI frame slot is still in native use.",
                                          "Retain the accepted frame and try another admitted free slot without waiting."};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"horo.render"}, ErrorCode{"render.ui_paint.frame.cancelled"},
                                            ErrorSeverity::Error, "The unpublished UI frame update was cancelled.",
                                            "Discard the candidate; the previously accepted slot remains unchanged."};
        const ErrorCodeDescriptor Unavailable{ErrorDomainId{"horo.render"}, ErrorCode{"render.ui_paint.frame.unavailable"},
                                              ErrorSeverity::Error, "The selected native UI paint owner is unavailable.",
                                              "Prepare an explicitly supported owner before admitting UI frame updates."};
    }  // namespace UiPaintFrameErrors

    namespace {
        bool ValidVertex(const UiPaintVertex &vertex) noexcept {
            return std::ranges::all_of(vertex.position, [](float value) {
                return std::isfinite(value);
            }) && std::ranges::all_of(vertex.uv, [](float value) {
                return std::isfinite(value);
            }) && std::ranges::all_of(vertex.color, [](float value) {
                return std::isfinite(value) && value >= 0 && value <= 1;
            });
        }

        bool ValidParameters(const UiPaintParameters &parameters) noexcept {
            return std::ranges::all_of(parameters.extent,
                                       [](float value) {
                return std::isfinite(value) && value > 0;
            }) && std::isfinite(parameters.opacity) &&
                   parameters.opacity >= 0 && parameters.opacity <= 1 && parameters.flags <= 7 &&
                   parameters.sampling <= static_cast<std::uint32_t>(UiPaintSampling::RoundedRectangle) &&
                   std::ranges::all_of(parameters.reserved, [](std::uint32_t value) {
                return value == 0;
            }) && std::ranges::all_of(parameters.maskReserved, [](std::uint32_t value) {
                return value == 0;
            }) && Detail::ValidUiPaintMask(parameters.maskExtent, parameters.cornerRadii);
        }
    }  // namespace

    /** @copydoc ValidateUiPaintFrameUpdate */
    Result<void> ValidateUiPaintFrameUpdate(const UiPaintFrameUpdate &update) {
        if (update.revision == 0 || update.vertices.empty() || update.indices.empty() || update.parameters.empty() ||
            update.vertices.size() > UiPaintLimits::MaximumVertices || update.indices.size() > UiPaintLimits::MaximumIndices ||
            update.parameters.size() > UiPaintLimits::MaximumDraws || !std::ranges::all_of(update.vertices, ValidVertex) ||
            !std::ranges::all_of(update.parameters, ValidParameters) || !std::ranges::all_of(update.indices, [&](std::uint32_t index) {
            return index < update.vertices.size();
        }))
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput, "Invalid complete UI paint frame payload."));
        return Result<void>::Success();
    }

    /** @copydoc IRenderBackend::UpdateUiPaintFrame */
    Result<void> IRenderBackend::UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &) {
        return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
    }
}  // namespace Horo::Render
