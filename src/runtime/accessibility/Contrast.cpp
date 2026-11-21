#include "Horo/Accessibility/Contrast.h"

#include <algorithm>
#include <cmath>

namespace Horo::Accessibility {
    namespace {
        /** @brief Decodes normalized sRGB into relative linear intensity for WCAG luminance. */
        double LinearChannel(const double value) noexcept {
            if (value <= 0.04045)
                return value / 12.92;
            return std::pow((value + 0.055) / 1.055, 2.4);
        }

        /** @brief Calculates WCAG relative luminance of validated opaque sRGB. */
        double Luminance(const SrgbColor color) noexcept {
            return 0.2126 * LinearChannel(color.red) + 0.7152 * LinearChannel(color.green) + 0.0722 * LinearChannel(color.blue);
        }
    }  // namespace

    /** @copydoc SrgbColor::IsValid */
    bool SrgbColor::IsValid() const noexcept {
        const auto valid = [](const double v) {
            return std::isfinite(v) && v >= 0 && v <= 1;
        };
        return valid(red) && valid(green) && valid(blue);
    }

    /** @copydoc EvaluateTextContrast */
    Result<TextContrast> EvaluateTextContrast(const SrgbColor foreground, const SrgbColor background, const ContrastTextSize size) {
        if (!foreground.IsValid() || !background.IsValid() || size > ContrastTextSize::Large)
            return Result<TextContrast>::Failure(MakeError(ContrastErrors::InvalidColor));
        const auto first = Luminance(foreground);
        const auto second = Luminance(background);
        const auto ratio = (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
        double aa = 4.5;
        double aaa = 7;
        if (size == ContrastTextSize::Large) {
            aa = 3;
            aaa = 4.5;
        }
        return Result<TextContrast>::Success({ratio, ratio >= aa, ratio >= aaa});
    }

    namespace ContrastErrors {
        /** @copydoc InvalidColor */
        const ErrorCodeDescriptor InvalidColor{ErrorDomainId{"horo.visual_accessibility"},
                                               ErrorCode{"visual_accessibility.color.invalid"},
                                               ErrorSeverity::Error,
                                               "Contrast input is invalid.",
                                               "Supply composed normalized sRGB colors and a supported text category.",
                                               false,
                                               false};
    }  // namespace ContrastErrors
}  // namespace Horo::Accessibility
