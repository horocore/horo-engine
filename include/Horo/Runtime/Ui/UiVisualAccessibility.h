#pragma once

/** @file UiVisualAccessibility.h
 * @brief UI-owned immutable scale/contrast preferences and readable typography projection.
 */
#include "Horo/Foundation/ModuleHost.h"

namespace Horo::Runtime::Ui {
    /** @brief Frame-captured UI preferences; no renderer quality, theme or bus owns these values. */
    class UiVisualAccessibilitySnapshot final {
    public:
        /** @brief Captures and validates UI visual settings from the frame's authority.
         * @param snapshot Snapshot with the UI contribution registered.
         * @return Typed view or UiErrors::VisualAccessibilityInvalid, preserving last-good consumer state on failure.
         */
        [[nodiscard]] static Result<UiVisualAccessibilitySnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @brief Returns desired layout/font scale, applied once by the owning DPI-aware UI composition.
         * @return Scale in [0.5,3]; capturing alone does not rebuild a font atlas or change native geometry.
         */
        [[nodiscard]] float UiScale() const noexcept;
        /** @brief Returns desired high contrast. @return Same-revision UI preference. */
        [[nodiscard]] bool HighContrast() const noexcept;
        /** @brief Projects one authored visible text role with the editor's readable floor.
         * @param authoredDips Finite positive size in logical DIPs, before user scale.
         * @return Scaled size, never below 16 DIP, or a typed invalid/overflow failure.
         * @details This is a size policy input to the actual font/layout owner, not a drawing bypass.
         */
        [[nodiscard]] Result<float> TextSize(float authoredDips) const;
        /** @brief Returns captured authority identity. @return Configuration revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;

    private:
        UiVisualAccessibilitySnapshot(ConfigurationSnapshot snapshot, float scale, bool highContrast) noexcept;
        ConfigurationSnapshot snapshot_;
        float scale_{1};
        bool highContrast_{};
    };

    /** @brief Returns inert descriptors exclusively for accessibility.visual.ui.*.
     * @return User/NextFrame contribution; ui_scale uses integer thousandths [500,3000].
     * @details No flat-key aliases or source precedence resolver are installed.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeUiVisualAccessibilityContribution();
}  // namespace Horo::Runtime::Ui
