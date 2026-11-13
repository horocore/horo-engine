#include "Horo/Accessibility/VisualAccessibility.h"

namespace Horo::Accessibility {
    namespace {
        /** @brief Initializes inert key metadata during composition; subsequent snapshot projection does not allocate keys. */
        const std::array<SettingKey, 12> &Keys() {
            static const std::array<SettingKey, 12>
                keys{SettingKey{"accessibility.colorblind.mode"},       SettingKey{"accessibility.colorblind.severity"},
                     SettingKey{"accessibility.colorblind.custom.m00"}, SettingKey{"accessibility.colorblind.custom.m01"},
                     SettingKey{"accessibility.colorblind.custom.m02"}, SettingKey{"accessibility.colorblind.custom.m10"},
                     SettingKey{"accessibility.colorblind.custom.m11"}, SettingKey{"accessibility.colorblind.custom.m12"},
                     SettingKey{"accessibility.colorblind.custom.m20"}, SettingKey{"accessibility.colorblind.custom.m21"},
                     SettingKey{"accessibility.colorblind.custom.m22"}, SettingKey{"accessibility.visual.ui.high_contrast"}};
            return keys;
        }
    }  // namespace

    /** @copydoc MakeColorAccessibilityContribution */
    ModuleConfigurationContribution MakeColorAccessibilityContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.accessibility.color"},
                                                     .ownerPrefix = "accessibility.colorblind"};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < 11; ++i) {
            std::int64_t value = 0;
            if (i == 1)
                value = 1000;
            else if (i >= 2 && (i - 2) % 4 == 0)
                value = 1000000;
            contribution.settings.push_back(
                {keys[i], SettingValueType::Integer, value, SettingScope::User, ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc ColorAccessibilitySnapshot::Capture */
    Result<ColorAccessibilitySnapshot> ColorAccessibilitySnapshot::Capture(ConfigurationSnapshot snapshot) {
        std::array<std::int64_t, 11> values{};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto *entry = snapshot.FindResolved(keys[i]);
            const auto *value = entry ? std::get_if<std::int64_t>(&entry->value) : nullptr;
            if (value == nullptr)
                return Result<ColorAccessibilitySnapshot>::Failure(MakeError(VisualAccessibilityErrors::InvalidSettings));
            values[i] = *value;
        }
        const auto *contrastEntry = snapshot.FindResolved(keys.back());
        const auto *contrast = contrastEntry ? std::get_if<bool>(&contrastEntry->value) : nullptr;
        if (contrast == nullptr || values[0] < 0 || values[0] > static_cast<std::int64_t>(ColorblindMode::CustomMatrix) || values[1] < 0 ||
            values[1] > 1000)
            return Result<ColorAccessibilitySnapshot>::Failure(MakeError(VisualAccessibilityErrors::InvalidSettings));
        ColorblindSettings settings{static_cast<ColorblindMode>(values[0]), static_cast<float>(values[1]) / 1000, {}};
        for (std::size_t i = 0; i < settings.custom.values.size(); ++i) {
            if (values[i + 2] < -4000000 || values[i + 2] > 4000000)
                return Result<ColorAccessibilitySnapshot>::Failure(MakeError(VisualAccessibilityErrors::InvalidSettings));
            settings.custom.values[i] = static_cast<float>(values[i + 2]) / 1000000;
        }
        return Result<ColorAccessibilitySnapshot>::Success(ColorAccessibilitySnapshot{std::move(snapshot), settings, *contrast});
    }

    /** @copydoc ColorAccessibilitySnapshot::ColorAccessibilitySnapshot */
    ColorAccessibilitySnapshot::ColorAccessibilitySnapshot(ConfigurationSnapshot snapshot, const ColorblindSettings settings,
                                                           const bool highContrast) noexcept
        : snapshot_(std::move(snapshot)), settings_(settings), highContrast_(highContrast) {}

    /** @copydoc ColorAccessibilitySnapshot::GetActiveColorblindMode */
    ColorblindMode ColorAccessibilitySnapshot::GetActiveColorblindMode() const noexcept {
        return settings_.mode;
    }

    /** @copydoc ColorAccessibilitySnapshot::GetColorblindSeverity */
    float ColorAccessibilitySnapshot::GetColorblindSeverity() const noexcept {
        return settings_.severity;
    }

    /** @copydoc ColorAccessibilitySnapshot::IsHighContrastEnabled */
    bool ColorAccessibilitySnapshot::IsHighContrastEnabled() const noexcept {
        return highContrast_;
    }

    /** @copydoc ColorAccessibilitySnapshot::Revision */
    ConfigurationRevision ColorAccessibilitySnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc ColorAccessibilitySnapshot::Settings */
    const ColorblindSettings &ColorAccessibilitySnapshot::Settings() const noexcept {
        return settings_;
    }

    /** @copydoc ColorAccessibilitySnapshot::Configuration */
    const ConfigurationSnapshot &ColorAccessibilitySnapshot::Configuration() const noexcept {
        return snapshot_;
    }
}  // namespace Horo::Accessibility
