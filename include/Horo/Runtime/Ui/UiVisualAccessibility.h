#pragma once

/** @file UiVisualAccessibility.h
 * @brief UI-owned immutable scale/contrast preferences and readable typography projection.
 */
#include "Horo/Foundation/ModuleHost.h"

#include <span>

namespace Horo::Runtime::Ui {
    struct UiDrawCommand;
    struct UiVisualStateMask;

    /** @brief Worst-case shared budgets for one control's persistent non-color state indicators. */
    struct UiVisualIndicatorLimits final {
        static constexpr std::uint32_t ControlCommands = 18;                       /**< Solid draws for simultaneous control states. */
        static constexpr std::uint32_t FocusCommands = 2;                          /**< Ordered dual-tone borders. */
        static constexpr std::uint32_t FocusQuads = 4 * FocusCommands;             /**< Realized border geometry. */
        static constexpr std::uint32_t Commands = ControlCommands + FocusCommands; /**< Total caller scratch envelope. */
    };

    /** @brief Reports whether the state carries supported non-color visual cues.
     * @param state Exact current style-state evidence. @return Whether focus or a control marker is present.
     */
    [[nodiscard]] bool HasUiVisualIndicators(UiVisualStateMask state) noexcept;

    /** @brief Projects visible immutable control state into backend-neutral non-color indicators.
     * @param state Exact same-frame visual-state evidence from the UI style owner.
     * @param surface Final logical bounds and inherited transform, clip, mask, group and opacity.
     * @param output Caller-owned scratch space with at least UiVisualIndicatorLimits::Commands entries.
     * @return Count written or typed capacity/layout error before writing. Empty bounds and invisible surfaces emit no indicators.
     * @details Focus uses a dual-tone outline. Control states use contrasting geometric marks independent of the authored palette.
     * A visible surface keeps opaque marks in normal and high-contrast styles. The owner's existing clip, mask and group still apply.
     */
    [[nodiscard]] Result<std::size_t> ProjectUiVisualIndicators(UiVisualStateMask state, const UiDrawCommand &surface,
                                                                std::span<UiDrawCommand> output);

    /** @brief One immutable configuration revision projected to Runtime UI presentation preferences.
     * @details The host captures both flags from the same configuration. Colours and focus treatments are selected by the
     * ordinary registered style cascade; the UI never changes input focus or visual-safety settings.
     */
    struct UiVisualPresentationPolicy final {
        ConfigurationRevision revision{};
        bool highContrast{};
        bool reduceMotion{};
        /** @brief Validates a new policy against one exact current value, where revision zero is unconfigured.
         * @param current Last admitted value.
         * @return Success for newer or exactly equal evidence, typed malformed, conflicting or stale failure otherwise.
         */
        [[nodiscard]] Result<void> ValidateReplacement(const UiVisualPresentationPolicy &current) const;
        [[nodiscard]] auto operator<=>(const UiVisualPresentationPolicy &) const noexcept = default;
    };

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
