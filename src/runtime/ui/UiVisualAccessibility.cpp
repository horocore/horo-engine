#include "Horo/Runtime/Ui/UiVisualAccessibility.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Horo::Runtime::Ui::UiErrors {
    /** @copydoc VisualAccessibilityInvalid */
    const ErrorCodeDescriptor VisualAccessibilityInvalid{ErrorDomainId{"horo.runtime_ui"},
                                                         ErrorCode{"runtime_ui.visual_accessibility.invalid"},
                                                         ErrorSeverity::Error,
                                                         "UI accessibility preferences or text size are invalid.",
                                                         "Compose UI visual settings and use finite readable sizes within the scale range.",
                                                         false,
                                                         false};
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Inert UI-owned keys, initialized before frame capture by explicit contribution composition. */
        const std::array<SettingKey, 2> &Keys() {
            static const std::array<SettingKey, 2> keys{SettingKey{"accessibility.visual.ui.ui_scale"},
                                                        SettingKey{"accessibility.visual.ui.high_contrast"}};
            return keys;
        }
    }  // namespace

    /** @copydoc MakeUiVisualAccessibilityContribution */
    ModuleConfigurationContribution MakeUiVisualAccessibilityContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.runtime.ui.accessibility"},
                                                     .ownerPrefix = "accessibility.visual.ui"};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const SettingValue value = i == 0 ? SettingValue{std::int64_t{1000}} : SettingValue{false};
            contribution.settings.push_back(
                {keys[i], i == 0 ? SettingValueType::Integer : SettingValueType::Boolean, value, SettingScope::User,
                 ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::Capture */
    Result<UiVisualAccessibilitySnapshot> UiVisualAccessibilitySnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto &keys = Keys();
        const auto *scaleEntry = snapshot.FindResolved(keys[0]);
        const auto *contrastEntry = snapshot.FindResolved(keys[1]);
        const auto *scale = scaleEntry ? std::get_if<std::int64_t>(&scaleEntry->value) : nullptr;
        const auto *contrast = contrastEntry ? std::get_if<bool>(&contrastEntry->value) : nullptr;
        if (!scale || !contrast || *scale < 500 || *scale > 3000)
            return Result<UiVisualAccessibilitySnapshot>::Failure(MakeError(UiErrors::VisualAccessibilityInvalid));
        const float multiplier = static_cast<float>(*scale) / 1000;
        const bool highContrast = *contrast;
        return Result<UiVisualAccessibilitySnapshot>::Success(UiVisualAccessibilitySnapshot{std::move(snapshot), multiplier, highContrast});
    }

    /** @copydoc UiVisualAccessibilitySnapshot::UiVisualAccessibilitySnapshot */
    UiVisualAccessibilitySnapshot::UiVisualAccessibilitySnapshot(ConfigurationSnapshot snapshot, const float scale,
                                                                 const bool highContrast) noexcept
        : snapshot_(std::move(snapshot)), scale_(scale), highContrast_(highContrast) {}

    /** @copydoc UiVisualAccessibilitySnapshot::UiScale */
    float UiVisualAccessibilitySnapshot::UiScale() const noexcept {
        return scale_;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::HighContrast */
    bool UiVisualAccessibilitySnapshot::HighContrast() const noexcept {
        return highContrast_;
    }

    /** @copydoc UiVisualAccessibilitySnapshot::Revision */
    ConfigurationRevision UiVisualAccessibilitySnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc UiVisualAccessibilitySnapshot::TextSize */
    Result<float> UiVisualAccessibilitySnapshot::TextSize(const float authoredDips) const {
        if (!std::isfinite(authoredDips) || authoredDips <= 0 || authoredDips > std::numeric_limits<float>::max() / 3)
            return Result<float>::Failure(MakeError(UiErrors::VisualAccessibilityInvalid));
        return Result<float>::Success(std::max(16.0F, authoredDips * scale_));
    }
}  // namespace Horo::Runtime::Ui
