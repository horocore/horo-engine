#pragma once

/** @file UiCaptionSettings.h
 * @brief Caption-owned inert configuration descriptors and frame-stable appearance projection.
 */
#include "Horo/Foundation/ModuleHost.h"
#include "Horo/Runtime/Ui/UiCaptions.h"

namespace Horo::Runtime::Ui {
    /** @brief One immutable caption appearance revision; configuration and provenance remain authoritative. */
    class CaptionSettingsSnapshot final {
    public:
        /** @brief Captures typed appearance at the HUD frame boundary.
         * @param snapshot Authority with the caption contribution registered.
         * @return Complete appearance or UiErrors::CaptionInvalid; retain last-good UI on failure.
         * @details Scale/opacity use integer thousandths, margin uses thousandths of a percent,
         * colors are packed RRGGBBAA integers and position is the closed CaptionPosition enum.
         */
        [[nodiscard]] static Result<CaptionSettingsSnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @brief Returns one frame's typed appearance. @return Borrowed immutable values. */
        [[nodiscard]] const CaptionAppearance &Appearance() const noexcept;
        /** @brief Returns captured configuration identity. @return Exact immutable revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;
        /** @brief Borrows authoritative provenance for this frame. @return Immutable snapshot lease. */
        [[nodiscard]] const ConfigurationSnapshot &Configuration() const noexcept;

    private:
        CaptionSettingsSnapshot(ConfigurationSnapshot snapshot, CaptionAppearance appearance) noexcept;
        ConfigurationSnapshot snapshot_;
        CaptionAppearance appearance_;
    };

    /** @brief Creates inert User/NextFrame descriptors exclusively for accessibility.captions.*.
     * @return Caption presentation contribution; does not register, activate or inspect a service.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeCaptionSettingsContribution();
}  // namespace Horo::Runtime::Ui
