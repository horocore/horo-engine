#pragma once

/** @file AccessibilitySettings.h
 * @brief Platform-owned desired screen-reader preference, independent of native availability.
 */
#include "Horo/Foundation/ModuleHost.h"

namespace Horo::Platform {
    /** @brief One immutable desired preference revision; Unsupported never rewrites this value. */
    class ScreenReaderSettingsSnapshot final {
    public:
        /** @brief Captures the platform domain's desired preference at the UI frame boundary.
         * @param snapshot Configuration authority with the screen-reader contribution registered.
         * @return Typed view or AccessibilitySettingsErrors::InvalidConfiguration.
         */
        [[nodiscard]] static Result<ScreenReaderSettingsSnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @brief Returns desired enablement, not actual native support. @return Captured user preference. */
        [[nodiscard]] bool Enabled() const noexcept;
        /** @brief Returns authoritative revision. @return Exact retained configuration revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;

    private:
        ScreenReaderSettingsSnapshot(ConfigurationSnapshot snapshot, bool enabled) noexcept;
        ConfigurationSnapshot snapshot_;
        bool enabled_{};
    };

    /** @brief Returns inert User/NextFrame descriptors exclusively for accessibility.screen_reader.*.
     * @return Platform-owned contribution; performs no native discovery, registration or activation.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeScreenReaderSettingsContribution();

    namespace AccessibilitySettingsErrors {
        /** @brief Desired screen-reader setting is absent or has the wrong type. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
    }  // namespace AccessibilitySettingsErrors
}  // namespace Horo::Platform
