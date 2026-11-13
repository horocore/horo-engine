#include "Horo/Platform/AccessibilitySettings.h"

namespace Horo::Platform {
    namespace {
        /** @brief Inert metadata initialized during explicit domain composition. */
        const SettingKey &EnabledKey() {
            static const SettingKey key{"accessibility.screen_reader.enabled"};
            return key;
        }
    }  // namespace

    /** @copydoc MakeScreenReaderSettingsContribution */
    ModuleConfigurationContribution MakeScreenReaderSettingsContribution() {
        return {.module = ModuleId{"horo.platform.accessibility"},
                .ownerPrefix = "accessibility.screen_reader",
                .settings = {{EnabledKey(), SettingValueType::Boolean, false, SettingScope::User, ReloadPolicy::NextFrame,
                              SettingSensitivity::Public,
                              ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                                            ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                                        false}}}};
    }

    /** @copydoc ScreenReaderSettingsSnapshot::Capture */
    Result<ScreenReaderSettingsSnapshot> ScreenReaderSettingsSnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto *entry = snapshot.FindResolved(EnabledKey());
        const auto *enabled = entry ? std::get_if<bool>(&entry->value) : nullptr;
        if (!enabled)
            return Result<ScreenReaderSettingsSnapshot>::Failure(MakeError(AccessibilitySettingsErrors::InvalidConfiguration));
        const bool desired = *enabled;
        return Result<ScreenReaderSettingsSnapshot>::Success(ScreenReaderSettingsSnapshot{std::move(snapshot), desired});
    }

    /** @copydoc ScreenReaderSettingsSnapshot::ScreenReaderSettingsSnapshot */
    ScreenReaderSettingsSnapshot::ScreenReaderSettingsSnapshot(ConfigurationSnapshot snapshot, const bool enabled) noexcept
        : snapshot_(std::move(snapshot)), enabled_(enabled) {}

    /** @copydoc ScreenReaderSettingsSnapshot::Enabled */
    bool ScreenReaderSettingsSnapshot::Enabled() const noexcept {
        return enabled_;
    }

    /** @copydoc ScreenReaderSettingsSnapshot::Revision */
    ConfigurationRevision ScreenReaderSettingsSnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    namespace AccessibilitySettingsErrors {
        /** @copydoc InvalidConfiguration */
        const ErrorCodeDescriptor InvalidConfiguration{ErrorDomainId{"horo.platform.accessibility"},
                                                       ErrorCode{"platform.accessibility.settings_invalid"},
                                                       ErrorSeverity::Error,
                                                       "Screen-reader preference is invalid.",
                                                       "Compose the platform setting and use its declared boolean value.",
                                                       false,
                                                       false};
    }  // namespace AccessibilitySettingsErrors
}  // namespace Horo::Platform
