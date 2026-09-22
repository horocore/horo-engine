#include "Horo/Runtime/Ui/UiCanvasSpace.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <numeric>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsRenderMode(const UiRenderMode mode) noexcept {
            using enum UiRenderMode;
            return mode == ScreenSpaceOverlay || mode == ScreenSpaceCamera || mode == WorldSpace;
        }

        [[nodiscard]] bool IsScaleMode(const UiScaleMode mode) noexcept {
            using enum UiScaleMode;
            return mode == ScaleWithScreenSize || mode == ConstantPixelSize || mode == ConstantPhysicalSize;
        }

        [[nodiscard]] bool IsSafeAreaMode(const UiSafeAreaMode mode) noexcept {
            using enum UiSafeAreaMode;
            return mode == Ignore || mode == Inset;
        }

        [[nodiscard]] bool IsPixelSnapMode(const UiPixelSnapMode mode) noexcept {
            using enum UiPixelSnapMode;
            return mode == Disabled || mode == Edges;
        }

        [[nodiscard]] UiCanvasScaleFactor ReducedFactor(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
            const std::uint64_t divisor = std::gcd(numerator, denominator);
            return {static_cast<std::uint32_t>(numerator / divisor), static_cast<std::uint32_t>(denominator / divisor)};
        }

        [[nodiscard]] UiCanvasDeviceScale ReducedScale(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
            const std::uint64_t divisor = std::gcd(numerator, denominator);
            return {static_cast<std::uint32_t>(numerator / divisor), static_cast<std::uint32_t>(denominator / divisor)};
        }

        [[nodiscard]] bool IsUnavailable(const UiCanvasDeviceScale scale) noexcept {
            return scale.pixelUnits == 0 && scale.logicalDips == 0;
        }

        [[nodiscard]] bool IsInside(const UiCanvasPixelRect rect, const UiCanvasPixelExtent extent) noexcept {
            return rect.IsValid() && static_cast<std::uint64_t>(rect.x) + rect.width <= extent.width &&
                   static_cast<std::uint64_t>(rect.y) + rect.height <= extent.height;
        }

        [[nodiscard]] Result<UiCanvasPixelRect> ResolveContentRect(const UiCanvasDescriptor &canvas,
                                                                   const UiCanvasViewportEvidence &evidence) {
            if (canvas.presentation.safeArea == UiSafeAreaMode::Ignore)
                return Result<UiCanvasPixelRect>::Success({});

            const auto &insets = evidence.safeAreaInsets;
            const auto horizontal = static_cast<std::uint64_t>(insets.left) + insets.right;
            const auto vertical = static_cast<std::uint64_t>(insets.top) + insets.bottom;
            if (horizontal >= evidence.pixelExtent.width || vertical >= evidence.pixelExtent.height)
                return Failure<UiCanvasPixelRect>(UiErrors::CanvasSpaceInvalid);
            if (insets == UiCanvasPixelInsets{})
                return Result<UiCanvasPixelRect>::Success({});
            return Result<UiCanvasPixelRect>::Success({insets.left, insets.top,
                                                       evidence.pixelExtent.width - static_cast<std::uint32_t>(horizontal),
                                                       evidence.pixelExtent.height - static_cast<std::uint32_t>(vertical)});
        }

        [[nodiscard]] Result<UiCanvasDeviceScale> ApplyUiScale(const UiCanvasDeviceScale base, const UiCanvasScaleFactor factor) {
            if (!base.IsValid() || !factor.IsValid())
                return Failure<UiCanvasDeviceScale>(UiErrors::CanvasSpaceInvalid);
            if (base.pixelUnits > std::numeric_limits<std::uint64_t>::max() / factor.numerator ||
                base.logicalDips > std::numeric_limits<std::uint64_t>::max() / factor.denominator)
                return Failure<UiCanvasDeviceScale>(UiErrors::CanvasSpaceOverflow);
            const std::uint64_t numerator = static_cast<std::uint64_t>(base.pixelUnits) * factor.numerator;
            const std::uint64_t denominator = static_cast<std::uint64_t>(base.logicalDips) * factor.denominator;
            const auto divisor = std::gcd(numerator, denominator);
            if (numerator / divisor > std::numeric_limits<std::uint32_t>::max() ||
                denominator / divisor > std::numeric_limits<std::uint32_t>::max())
                return Failure<UiCanvasDeviceScale>(UiErrors::CanvasSpaceOverflow);
            return Result<UiCanvasDeviceScale>::Success(
                {static_cast<std::uint32_t>(numerator / divisor), static_cast<std::uint32_t>(denominator / divisor)});
        }

        [[nodiscard]] Result<UiCanvasDeviceScale> ResolveBaseScale(const UiCanvasDescriptor &canvas,
                                                                   const UiCanvasViewportEvidence &evidence) {
            using enum UiScaleMode;
            switch (canvas.scaleMode) {
                case ScaleWithScreenSize: {
                    const std::uint64_t widthCross =
                        static_cast<std::uint64_t>(evidence.pixelExtent.width) * canvas.referenceResolution.height;
                    const std::uint64_t heightCross =
                        static_cast<std::uint64_t>(evidence.pixelExtent.height) * canvas.referenceResolution.width;
                    return Result<UiCanvasDeviceScale>::Success(
                        widthCross <= heightCross ? ReducedScale(evidence.pixelExtent.width, canvas.referenceResolution.width)
                                                  : ReducedScale(evidence.pixelExtent.height, canvas.referenceResolution.height));
                }
                case ConstantPixelSize:
                    return Result<UiCanvasDeviceScale>::Success({1, 1});
                case ConstantPhysicalSize:
                    if (!evidence.dpiScale.IsValid())
                        return Failure<UiCanvasDeviceScale>(UiErrors::CanvasSpaceInvalid);
                    return Result<UiCanvasDeviceScale>::Success(ReducedScale(evidence.dpiScale.pixelUnits, evidence.dpiScale.logicalDips));
            }
            return Failure<UiCanvasDeviceScale>(UiErrors::CanvasSpaceInvalid);
        }

        [[nodiscard]] Result<std::int32_t> ResolveAxis(const std::uint32_t pixels, const UiCanvasDeviceScale scale) noexcept {
            constexpr std::uint64_t UnitsPerDip = 64;
            const std::uint64_t scaledPixels = static_cast<std::uint64_t>(pixels) * scale.logicalDips;
            if (scaledPixels > std::numeric_limits<std::uint64_t>::max() / UnitsPerDip)
                return Failure<std::int32_t>(UiErrors::CanvasSpaceOverflow);
            const std::uint64_t numerator = scaledPixels * UnitsPerDip;
            const std::uint64_t quotient = numerator / scale.pixelUnits;
            const std::uint64_t remainder = numerator % scale.pixelUnits;
            const std::uint64_t halfway = scale.pixelUnits / 2;
            const bool aboveHalf = remainder > halfway;
            const bool exactHalf = (scale.pixelUnits % 2 == 0) && remainder == halfway;
            const std::uint64_t rounded = quotient + static_cast<std::uint64_t>(aboveHalf || (exactHalf && (quotient % 2 != 0)));
            if (rounded > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                return Failure<std::int32_t>(UiErrors::CanvasSpaceOverflow);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(rounded));
        }
    }  // namespace

    /** @copydoc UiCanvasReferenceResolution::IsValid */
    bool UiCanvasReferenceResolution::IsValid() const noexcept {
        return width > 0 && height > 0 && width <= MaximumUiCanvasReferenceDip && height <= MaximumUiCanvasReferenceDip;
    }

    /** @copydoc UiCanvasScaleFactor::IsValid */
    bool UiCanvasScaleFactor::IsValid() const noexcept {
        return numerator > 0 && denominator > 0 && numerator <= MaximumUiCanvasScaleFactorComponent &&
               denominator <= MaximumUiCanvasScaleFactorComponent;
    }

    /** @copydoc UiCanvasPixelExtent::IsValid */
    bool UiCanvasPixelExtent::IsValid() const noexcept {
        return width > 0 && height > 0;
    }

    /** @copydoc UiCanvasDeviceScale::IsValid */
    bool UiCanvasDeviceScale::IsValid() const noexcept {
        return pixelUnits > 0 && logicalDips > 0;
    }

    /** @copydoc UiCanvasPixelRect::IsValid */
    bool UiCanvasPixelRect::IsValid() const noexcept {
        return width > 0 && height > 0;
    }

    /** @copydoc UiCanvasPresentationPolicy::IsValid */
    bool UiCanvasPresentationPolicy::IsValid() const noexcept {
        return IsSafeAreaMode(safeArea) && uiScale.IsValid() && fontScale.IsValid() && IsPixelSnapMode(pixelSnap);
    }

    /** @copydoc UiCanvasViewportEvidence::IsValid */
    bool UiCanvasViewportEvidence::IsValid() const noexcept {
        return pixelExtent.IsValid() && (IsUnavailable(dpiScale) || dpiScale.IsValid());
    }

    /** @copydoc ResolveUiScreenCanvas */
    Result<UiResolvedScreenCanvas> ResolveUiScreenCanvas(const UiCanvasDescriptor &canvas, const UiCanvasPixelExtent viewport,
                                                         const UiCanvasDeviceScale deviceScale, const UiCanvasPixelInsets safeAreaInsets) {
        return ResolveUiScreenCanvasWithEvidence(canvas, {viewport, deviceScale, safeAreaInsets});
    }

    /** @copydoc UiResolvedScreenCanvas::ContentPixelRect */
    UiCanvasPixelRect UiResolvedScreenCanvas::ContentPixelRect() const noexcept {
        if (contentPixelRect == UiCanvasPixelRect{})
            return {0, 0, pixelExtent.width, pixelExtent.height};
        return contentPixelRect;
    }

    /** @copydoc UiResolvedScreenCanvas::IsValid */
    bool UiResolvedScreenCanvas::IsValid() const noexcept {
        const auto content = ContentPixelRect();
        const auto horizontalInsets = static_cast<std::uint64_t>(safeAreaInsets.left) + safeAreaInsets.right;
        const auto verticalInsets = static_cast<std::uint64_t>(safeAreaInsets.top) + safeAreaInsets.bottom;
        const bool contentMatchesInsets = pixelExtent.IsValid() && horizontalInsets < pixelExtent.width &&
                                          verticalInsets < pixelExtent.height &&
                                          ((contentPixelRect == UiCanvasPixelRect{} && safeAreaInsets == UiCanvasPixelInsets{} &&
                                            content == UiCanvasPixelRect{0, 0, pixelExtent.width, pixelExtent.height}) ||
                                           (contentPixelRect != UiCanvasPixelRect{} &&
                                            content == UiCanvasPixelRect{safeAreaInsets.left, safeAreaInsets.top,
                                                                         pixelExtent.width - static_cast<std::uint32_t>(horizontalInsets),
                                                                         pixelExtent.height - static_cast<std::uint32_t>(verticalInsets)}));
        const bool dpiValid = IsUnavailable(dpiScale) || dpiScale.IsValid();
        return pixelExtent.IsValid() && pixelsPerDip.IsValid() && logicalExtent.width >= 0 && logicalExtent.height >= 0 &&
               IsInside(content, pixelExtent) && contentMatchesInsets && dpiValid && uiScale.IsValid() && fontScale.IsValid() &&
               IsPixelSnapMode(pixelSnap);
    }

    /** @copydoc UiCanvasDescriptor::IsValid */
    bool UiCanvasDescriptor::IsValid() const noexcept {
        return id.IsValid() && rootElement.IsValid() && IsRenderMode(renderMode) && referenceResolution.IsValid() &&
               IsScaleMode(scaleMode) && presentation.IsValid();
    }

    /** @copydoc ResolveUiScreenCanvasWithEvidence */
    Result<UiResolvedScreenCanvas> ResolveUiScreenCanvasWithEvidence(const UiCanvasDescriptor &canvas,
                                                                     const UiCanvasViewportEvidence &evidence) {
        if (!canvas.IsValid() || !evidence.IsValid())
            return Failure<UiResolvedScreenCanvas>(UiErrors::CanvasSpaceInvalid);
        if (canvas.renderMode == UiRenderMode::WorldSpace)
            return Failure<UiResolvedScreenCanvas>(UiErrors::CanvasSpaceModeMismatch);

        const auto baseScale = ResolveBaseScale(canvas, evidence);
        if (baseScale.HasError())
            return Result<UiResolvedScreenCanvas>::Failure(baseScale.ErrorValue());
        auto scale = ApplyUiScale(baseScale.Value(), canvas.presentation.uiScale);
        if (scale.HasError())
            return Result<UiResolvedScreenCanvas>::Failure(scale.ErrorValue());
        const auto content = ResolveContentRect(canvas, evidence);
        if (content.HasError())
            return Result<UiResolvedScreenCanvas>::Failure(content.ErrorValue());
        const auto contentRect = content.Value() == UiCanvasPixelRect{}
                                     ? UiCanvasPixelRect{0, 0, evidence.pixelExtent.width, evidence.pixelExtent.height}
                                     : content.Value();

        auto width = ResolveAxis(contentRect.width, scale.Value());
        if (width.HasError())
            return Result<UiResolvedScreenCanvas>::Failure(width.ErrorValue());
        auto height = ResolveAxis(contentRect.height, scale.Value());
        if (height.HasError())
            return Result<UiResolvedScreenCanvas>::Failure(height.ErrorValue());
        const auto dpiScale =
            evidence.dpiScale.IsValid() ? ReducedScale(evidence.dpiScale.pixelUnits, evidence.dpiScale.logicalDips) : UiCanvasDeviceScale{};
        const auto safeInsets = canvas.presentation.safeArea == UiSafeAreaMode::Inset ? evidence.safeAreaInsets : UiCanvasPixelInsets{};
        const auto uiScale = ReducedFactor(canvas.presentation.uiScale.numerator, canvas.presentation.uiScale.denominator);
        const auto fontScale = ReducedFactor(canvas.presentation.fontScale.numerator, canvas.presentation.fontScale.denominator);
        return Result<UiResolvedScreenCanvas>::Success({{std::move(width).Value(), std::move(height).Value()},
                                                        evidence.pixelExtent,
                                                        scale.Value(),
                                                        content.Value(),
                                                        safeInsets,
                                                        dpiScale,
                                                        uiScale,
                                                        fontScale,
                                                        canvas.presentation.pixelSnap});
    }

    /** @copydoc ResolveUiWorldCanvas */
    Result<UiCanvasLogicalExtent> ResolveUiWorldCanvas(const UiCanvasDescriptor &canvas) {
        if (!canvas.IsValid())
            return Failure<UiCanvasLogicalExtent>(UiErrors::CanvasSpaceInvalid);
        if (canvas.renderMode != UiRenderMode::WorldSpace)
            return Failure<UiCanvasLogicalExtent>(UiErrors::CanvasSpaceModeMismatch);
        if (canvas.presentation.safeArea != UiSafeAreaMode::Ignore || canvas.presentation.pixelSnap != UiPixelSnapMode::Disabled)
            return Failure<UiCanvasLogicalExtent>(UiErrors::CanvasSpaceModeMismatch);
        return Result<UiCanvasLogicalExtent>::Success({static_cast<std::int32_t>(canvas.referenceResolution.width * 64U),
                                                       static_cast<std::int32_t>(canvas.referenceResolution.height * 64U)});
    }
}  // namespace Horo::Runtime::Ui
