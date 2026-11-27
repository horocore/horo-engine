#pragma once

/** @file Contrast.h
 * @brief Display-independent WCAG numeric text contrast evaluation for composed sRGB.
 */
#include "Horo/Foundation/Result.h"

#include <cstdint>

namespace Horo::Accessibility {
    /** @brief Finite normalized sRGB encoded color after composition; alpha/unknown backdrop are excluded. */
    struct SrgbColor final {
        double red{}, green{}, blue{};
        /** @brief Checks finite normalized channels. @return Whether relative luminance is defined. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Text category supplied from actual font metrics, not inferred from a color pair. */
    enum class ContrastTextSize : std::uint8_t {
        Standard,
        Large
    };

    /** @brief Numeric WCAG contrast assessment, independent of full product compliance. */
    struct TextContrast final {
        double ratio{};
        bool meetsAA{};
        bool meetsAAA{};
    };

    /**
     * @brief Evaluates WCAG 2.2 text contrast using unrounded threshold comparisons.
     * @param foreground Actual composed text color in encoded sRGB.
     * @param background Actual opaque background color in encoded sRGB.
     * @param size Standard or large text as determined by the caller's actual font metrics.
     * @return Ratio and AA/AAA numeric outcomes, or InvalidColor for non-finite/out-of-range input.
     * @details Large means at least 18pt regular or 14pt bold. Standard thresholds are
     * 4.5/7, large thresholds 3/4.5. This check alone is not an accessibility certification.
     */
    [[nodiscard]] Result<TextContrast> EvaluateTextContrast(SrgbColor foreground, SrgbColor background,
                                                            ContrastTextSize size = ContrastTextSize::Standard);

    namespace ContrastErrors {
        /** @brief Contrast input is not finite normalized composed sRGB, or text category is invalid. */
        extern const ErrorCodeDescriptor InvalidColor;
    }  // namespace ContrastErrors
}  // namespace Horo::Accessibility
