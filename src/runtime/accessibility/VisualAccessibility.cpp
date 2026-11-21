#include "Horo/Accessibility/VisualAccessibility.h"

#include <algorithm>
#include <cmath>

namespace Horo::Accessibility {
    namespace {
        /** @brief Reference full-severity display-linear sRGB simulation coefficients from Machado et al. (2009). */
        constexpr ColorMatrix Protanopia{
            {0.152286F, 1.052583F, -0.204868F, 0.114503F, 0.786281F, 0.099216F, -0.003882F, -0.048116F, 1.051998F}};
        constexpr ColorMatrix Deuteranopia{
            {0.367322F, 0.860646F, -0.227968F, 0.280085F, 0.672501F, 0.047413F, -0.011820F, 0.042940F, 0.968881F}};
        constexpr ColorMatrix Tritanopia{
            {1.255528F, -0.076749F, -0.178779F, -0.078411F, 0.930809F, 0.147602F, 0.004733F, 0.691367F, 0.303900F}};
        constexpr ColorMatrix Achromatopsia{{0.2126F, 0.7152F, 0.0722F, 0.2126F, 0.7152F, 0.0722F, 0.2126F, 0.7152F, 0.0722F}};

    }  // namespace

    /** @copydoc ColorMatrix::IsValid */
    bool ColorMatrix::IsValid() const noexcept {
        return std::ranges::all_of(values, [](const float v) {
            return std::isfinite(v) && v >= -4 && v <= 4;
        });
    }

    /** @copydoc PrepareColorAccessibilityTransform */
    Result<ColorMatrix> PrepareColorAccessibilityTransform(const ColorblindSettings &settings) {
        if (!std::isfinite(settings.severity) || settings.severity < 0 || settings.severity > 1 || !settings.custom.IsValid())
            return Result<ColorMatrix>::Failure(MakeError(VisualAccessibilityErrors::InvalidSettings));
        ColorMatrix selected;
        switch (settings.mode) {
            case ColorblindMode::None:
                break;
            case ColorblindMode::Protanopia:
                selected = Protanopia;
                break;
            case ColorblindMode::Deuteranopia:
                selected = Deuteranopia;
                break;
            case ColorblindMode::Tritanopia:
                selected = Tritanopia;
                break;
            case ColorblindMode::Achromatopsia:
                selected = Achromatopsia;
                break;
            case ColorblindMode::CustomMatrix:
                selected = settings.custom;
                break;
            default:
                return Result<ColorMatrix>::Failure(MakeError(VisualAccessibilityErrors::InvalidSettings));
        }
        const ColorMatrix identity;
        for (std::size_t i = 0; i < selected.values.size(); ++i)
            selected.values[i] = std::lerp(identity.values[i], selected.values[i], settings.severity);
        return Result<ColorMatrix>::Success(selected);
    }

    namespace VisualAccessibilityErrors {
        /** @copydoc InvalidSettings */
        const ErrorCodeDescriptor InvalidSettings{ErrorDomainId{"horo.visual_accessibility"},
                                                  ErrorCode{"visual_accessibility.settings.invalid"},
                                                  ErrorSeverity::Error,
                                                  "Visual accessibility settings are invalid.",
                                                  "Use a supported mode, finite strength and bounded custom coefficients.",
                                                  false,
                                                  false};
    }  // namespace VisualAccessibilityErrors
}  // namespace Horo::Accessibility
