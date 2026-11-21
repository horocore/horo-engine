#include "Horo/Accessibility/VisualSafety.h"

#include <array>

namespace Horo::Accessibility {
    namespace {
        /** @brief Inert safety metadata initialized during host contribution composition. */
        const std::array<SettingKey, 3> &Keys() {
            static const std::array<SettingKey, 3> keys{SettingKey{"accessibility.visual.safety.reduce_motion"},
                                                        SettingKey{"accessibility.visual.safety.flash_suppression"},
                                                        SettingKey{"accessibility.visual.safety.camera_shake_multiplier"}};
            return keys;
        }
    }  // namespace

    /** @copydoc MakeVisualSafetyContribution */
    ModuleConfigurationContribution MakeVisualSafetyContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.accessibility.visual_safety"},
                                                     .ownerPrefix = "accessibility.visual.safety"};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const bool boolean = i < 2;
            const SettingValue value = boolean ? SettingValue{false} : SettingValue{std::int64_t{1000}};
            contribution.settings.push_back(
                {keys[i], boolean ? SettingValueType::Boolean : SettingValueType::Integer, value, SettingScope::User,
                 ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc VisualSafetySnapshot::Capture */
    Result<VisualSafetySnapshot> VisualSafetySnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto &keys = Keys();
        const auto *motionEntry = snapshot.FindResolved(keys[0]);
        const auto *flashEntry = snapshot.FindResolved(keys[1]);
        const auto *shakeEntry = snapshot.FindResolved(keys[2]);
        const auto *motion = motionEntry ? std::get_if<bool>(&motionEntry->value) : nullptr;
        const auto *flash = flashEntry ? std::get_if<bool>(&flashEntry->value) : nullptr;
        const auto *shake = shakeEntry ? std::get_if<std::int64_t>(&shakeEntry->value) : nullptr;
        if (!motion || !flash || !shake || *shake < 0 || *shake > 1000)
            return Result<VisualSafetySnapshot>::Failure(MakeError(VisualSafetyErrors::InvalidSettings));
        const bool reduceMotion = *motion;
        const bool suppressFlash = *flash;
        const float multiplier = static_cast<float>(*shake) / 1000;
        return Result<VisualSafetySnapshot>::Success(VisualSafetySnapshot{std::move(snapshot), reduceMotion, suppressFlash, multiplier});
    }

    /** @copydoc VisualSafetySnapshot::VisualSafetySnapshot */
    VisualSafetySnapshot::VisualSafetySnapshot(ConfigurationSnapshot snapshot, const bool reduceMotion, const bool suppressFlash,
                                               const float shake) noexcept
        : snapshot_(std::move(snapshot)), reduceMotion_(reduceMotion), suppressFlash_(suppressFlash), shake_(shake) {}

    /** @copydoc VisualSafetySnapshot::ReduceMotion */
    bool VisualSafetySnapshot::ReduceMotion() const noexcept {
        return reduceMotion_;
    }

    /** @copydoc VisualSafetySnapshot::SuppressFlash */
    bool VisualSafetySnapshot::SuppressFlash() const noexcept {
        return suppressFlash_;
    }

    /** @copydoc VisualSafetySnapshot::Revision */
    ConfigurationRevision VisualSafetySnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc VisualSafetySnapshot::Scale */
    float VisualSafetySnapshot::Scale(const VisualEffectIntent intent) const noexcept {
        switch (intent) {
            case VisualEffectIntent::CameraShake:
                return reduceMotion_ ? 0 : shake_;
            case VisualEffectIntent::Flash:
                return suppressFlash_ ? 0 : 1;
            case VisualEffectIntent::DecorativeMotion:
                return reduceMotion_ ? 0 : 1;
            case VisualEffectIntent::EssentialMotion:
                return 1;
            default:
                return 0;
        }
    }

    namespace VisualSafetyErrors {
        /** @copydoc InvalidSettings */
        const ErrorCodeDescriptor InvalidSettings{ErrorDomainId{"horo.visual_accessibility"},
                                                  ErrorCode{"visual_accessibility.safety.invalid"},
                                                  ErrorSeverity::Error,
                                                  "Visual safety settings are invalid.",
                                                  "Compose safety preferences and use a shake multiplier within the admitted range.",
                                                  false,
                                                  false};
    }  // namespace VisualSafetyErrors
}  // namespace Horo::Accessibility
