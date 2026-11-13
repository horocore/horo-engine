#pragma once

/** @file VisualAccessibility.h
 * @brief Backend-neutral color transforms and immutable desired-setting queries.
 */

#include "Horo/Foundation/ModuleHost.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstdint>

namespace Horo::Accessibility {
    /** @brief Desired color mode; device/backend availability does not change this preference. */
    enum class ColorblindMode : std::uint8_t {
        None,
        Protanopia,
        Deuteranopia,
        Tritanopia,
        Achromatopsia,
        CustomMatrix
    };

    /** @brief Row-major display-linear sRGB transform acting on RGB column vectors. */
    struct ColorMatrix final {
        std::array<float, 9> values{1, 0, 0, 0, 1, 0, 0, 0, 1};
        /** @brief Validates finite coefficients bounded to [-4,4]. @return Whether the matrix is admitted. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Typed desired settings projected by the owning configuration domain at a frame boundary. */
    struct ColorblindSettings final {
        ColorblindMode mode{ColorblindMode::None};
        float severity{1}; /**< Identity-to-selected-transform interpolation strength in [0,1]. */
        ColorMatrix custom;
    };

    /**
     * @brief Prepares a finite immutable numeric transform without renderer/UI dependencies.
     * @param settings One frame's desired settings; even inactive custom coefficients must be valid.
     * @return Row-major matrix or VisualAccessibilityErrors::InvalidSettings.
     * @details Protanopia/deuteranopia/tritanopia use the full-severity Machado et al. reference
     * matrices. Severity blends with identity, not a physiological anomalous-trichromacy model.
     * Apply in display-linear sRGB after output transform and composed UI, before transfer
     * encoding; other output primaries require explicit conversion. No gamut clamp is implied.
     */
    [[nodiscard]] Result<ColorMatrix> PrepareColorAccessibilityTransform(const ColorblindSettings &settings);

    /** @brief Read-only desired color semantics; all getters refer to one captured configuration revision. */
    class IColorAccessibilityQuery {
    public:
        virtual ~IColorAccessibilityQuery() = default;
        /** @brief Returns desired mode, independent of GPU completion. @return Captured mode. */
        [[nodiscard]] virtual ColorblindMode GetActiveColorblindMode() const noexcept = 0;
        /** @brief Returns interpolation strength. @return Captured normalized strength. */
        [[nodiscard]] virtual float GetColorblindSeverity() const noexcept = 0;
        /** @brief Returns the UI domain's desired contrast preference. @return Captured preference. */
        [[nodiscard]] virtual bool IsHighContrastEnabled() const noexcept = 0;
    };

    /** @brief Snapshot-backed synchronous query, safe for concurrent const reads and independent of renderer/UI libraries. */
    class ColorAccessibilitySnapshot final : public IColorAccessibilityQuery {
    public:
        /** @brief Captures and validates a single authoritative configuration revision.
         * @param snapshot Snapshot with the color contribution and separately owned UI high-contrast setting.
         * @return Captured view or InvalidSettings; caller retains its previous frame on failure.
         * @details Numeric wire values use integer fixed point: severity in thousandths and
         * custom coefficients in millionths. No string parsing, source resolution or aliases occur.
         */
        [[nodiscard]] static Result<ColorAccessibilitySnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @copydoc IColorAccessibilityQuery::GetActiveColorblindMode */
        [[nodiscard]] ColorblindMode GetActiveColorblindMode() const noexcept override;
        /** @copydoc IColorAccessibilityQuery::GetColorblindSeverity */
        [[nodiscard]] float GetColorblindSeverity() const noexcept override;
        /** @copydoc IColorAccessibilityQuery::IsHighContrastEnabled */
        [[nodiscard]] bool IsHighContrastEnabled() const noexcept override;
        /** @brief Returns captured authoritative revision. @return Exact retained revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;
        /** @brief Borrows projected typed settings. @return Settings valid for this view's lifetime. */
        [[nodiscard]] const ColorblindSettings &Settings() const noexcept;
        /** @brief Borrows the authority/provenance lease. @return Immutable configuration snapshot. */
        [[nodiscard]] const ConfigurationSnapshot &Configuration() const noexcept;

    private:
        ColorAccessibilitySnapshot(ConfigurationSnapshot snapshot, ColorblindSettings settings, bool highContrast) noexcept;
        ConfigurationSnapshot snapshot_;
        ColorblindSettings settings_;
        bool highContrast_{};
    };

    /** @brief Creates inert descriptors exclusively for accessibility.colorblind.*.
     * @return Color domain contribution with User scope and NextFrame reload; registers nothing.
     * @details Mode is the closed integer enum; severity is [0,1000]; custom m00..m22
     * are integer millionths in [-4000000,4000000]. Capture performs domain validation.
     * UI high contrast belongs to accessibility.visual.ui and is never registered here.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeColorAccessibilityContribution();

    namespace VisualAccessibilityErrors {
        /** @brief Color mode, strength or custom matrix violates the finite typed contract. */
        extern const ErrorCodeDescriptor InvalidSettings;
    }  // namespace VisualAccessibilityErrors
}  // namespace Horo::Accessibility
