#pragma once

#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::CookPresentationDetail {
    /** @brief Explicit binary compatibility window, independent of the durable source schema. */
    [[nodiscard]] inline bool IsSupportedFormat(const std::uint32_t version) noexcept {
        return version >= 1 && version <= CurrentCookedUiDocumentFormatVersion;
    }

    /** @brief Writes the bounded authored policy through the owning cooked writer. */
    template <typename Writer> [[nodiscard]] bool Write(Writer &writer, const UiCanvasPresentationPolicy &policy) {
        return writer.Byte(static_cast<std::uint8_t>(policy.safeArea)) && writer.U32(policy.uiScale.numerator) &&
               writer.U32(policy.uiScale.denominator) && writer.U32(policy.fontScale.numerator) &&
               writer.U32(policy.fontScale.denominator) && writer.Byte(static_cast<std::uint8_t>(policy.pixelSnap));
    }

    /** @brief Encodes each canvas and its complete authored policy in the same deterministic order. */
    template <typename Writer> [[nodiscard]] bool WriteCanvases(Writer &writer, std::span<const UiCanvasDescriptor> canvases) {
        for (const auto &canvas : canvases)
            if (!writer.Bytes(canvas.id.Bytes()) || !writer.Bytes(canvas.rootElement.Bytes()) ||
                !writer.Byte(static_cast<std::uint8_t>(canvas.renderMode)) || !writer.U32(canvas.referenceResolution.width) ||
                !writer.U32(canvas.referenceResolution.height) || !writer.Byte(static_cast<std::uint8_t>(canvas.scaleMode)) ||
                !Write(writer, canvas.presentation))
                return false;
        return true;
    }

    /** @brief Reads bounded v2 fields and validates the existing typed presentation contract. */
    template <typename Reader> [[nodiscard]] Result<UiCanvasPresentationPolicy> Read(Reader &reader) {
        UiCanvasPresentationPolicy policy;
        std::uint8_t safeArea{};
        std::uint8_t pixelSnap{};
        if (!reader.Byte(safeArea) || !reader.U32(policy.uiScale.numerator) || !reader.U32(policy.uiScale.denominator) ||
            !reader.U32(policy.fontScale.numerator) || !reader.U32(policy.fontScale.denominator) || !reader.Byte(pixelSnap))
            return Result<UiCanvasPresentationPolicy>::Failure(MakeError(UiErrors::CookedPayloadMalformed));
        policy.safeArea = static_cast<UiSafeAreaMode>(safeArea);
        policy.pixelSnap = static_cast<UiPixelSnapMode>(pixelSnap);
        return policy.IsValid() ? Result<UiCanvasPresentationPolicy>::Success(policy)
                                : Result<UiCanvasPresentationPolicy>::Failure(MakeError(UiErrors::CookedPayloadMalformed));
    }
}  // namespace Horo::Runtime::Ui::CookPresentationDetail
