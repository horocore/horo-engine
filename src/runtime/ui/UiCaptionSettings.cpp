#include "Horo/Runtime/Ui/UiCaptionSettings.h"

#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Inert keys initialized at explicit caption-domain composition, never reconstructed during frames. */
        const std::array<SettingKey, 10> &Keys() {
            static const std::array<SettingKey, 10> keys{SettingKey{"accessibility.captions.enabled"},
                                                         SettingKey{"accessibility.captions.font_size"},
                                                         SettingKey{"accessibility.captions.text_color"},
                                                         SettingKey{"accessibility.captions.background_color"},
                                                         SettingKey{"accessibility.captions.background_opacity"},
                                                         SettingKey{"accessibility.captions.position"},
                                                         SettingKey{"accessibility.captions.show_speaker"},
                                                         SettingKey{"accessibility.captions.show_sound_descriptions"},
                                                         SettingKey{"accessibility.captions.show_directional_indicators"},
                                                         SettingKey{"accessibility.captions.safe_area_margin_percent"}};
            return keys;
        }

        /** @brief Reads one exact descriptor value without source fallback or allocation. */
        template <typename T> const T *Value(const ConfigurationSnapshot &snapshot, const std::size_t index) noexcept {
            const auto *entry = snapshot.FindResolved(Keys()[index]);
            return entry ? std::get_if<T>(&entry->value) : nullptr;
        }

        /** @brief Decodes already range-checked packed RRGGBBAA into normalized components. */
        std::array<float, 4> Color(const std::int64_t encoded) noexcept {
            const auto value = static_cast<std::uint32_t>(encoded);
            return {static_cast<float>((value >> 24) & 255) / 255, static_cast<float>((value >> 16) & 255) / 255,
                    static_cast<float>((value >> 8) & 255) / 255, static_cast<float>(value & 255) / 255};
        }
    }  // namespace

    /** @copydoc MakeCaptionSettingsContribution */
    ModuleConfigurationContribution MakeCaptionSettingsContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.runtime.ui.captions"},
                                                     .ownerPrefix = "accessibility.captions"};
        const std::array<SettingValue, 10>
            defaults{false, std::int64_t{1000}, std::int64_t{0xffffffff}, std::int64_t{255}, std::int64_t{750}, std::int64_t{0}, true, true,
                     false, std::int64_t{5000}};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto type = std::holds_alternative<bool>(defaults[i]) ? SettingValueType::Boolean : SettingValueType::Integer;
            contribution.settings.push_back(
                {keys[i], type, defaults[i], SettingScope::User, ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc CaptionSettingsSnapshot::Capture */
    Result<CaptionSettingsSnapshot> CaptionSettingsSnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto *enabled = Value<bool>(snapshot, 0);
        const auto *scale = Value<std::int64_t>(snapshot, 1);
        const auto *text = Value<std::int64_t>(snapshot, 2);
        const auto *background = Value<std::int64_t>(snapshot, 3);
        const auto *opacity = Value<std::int64_t>(snapshot, 4);
        const auto *position = Value<std::int64_t>(snapshot, 5);
        const auto *speaker = Value<bool>(snapshot, 6);
        const auto *sound = Value<bool>(snapshot, 7);
        const auto *direction = Value<bool>(snapshot, 8);
        const auto *margin = Value<std::int64_t>(snapshot, 9);
        if (!enabled || !scale || !text || !background || !opacity || !position || !speaker || !sound || !direction || !margin ||
            *scale < 500 || *scale > 3000 || *text < 0 || *text > 0xffffffffLL || *background < 0 || *background > 0xffffffffLL ||
            *opacity < 0 || *opacity > 1000 || *position < 0 || *position > 2 || *margin < 0 || *margin > 25000)
            return Result<CaptionSettingsSnapshot>::Failure(MakeError(UiErrors::CaptionInvalid));
        CaptionAppearance appearance{*enabled,
                                     static_cast<float>(*scale) / 1000,
                                     Color(*text),
                                     Color(*background),
                                     static_cast<float>(*opacity) / 1000,
                                     static_cast<CaptionPosition>(*position),
                                     *speaker,
                                     *sound,
                                     *direction,
                                     static_cast<float>(*margin) / 1000};
        return Result<CaptionSettingsSnapshot>::Success(CaptionSettingsSnapshot{std::move(snapshot), appearance});
    }

    /** @copydoc CaptionSettingsSnapshot::CaptionSettingsSnapshot */
    CaptionSettingsSnapshot::CaptionSettingsSnapshot(ConfigurationSnapshot snapshot, const CaptionAppearance appearance) noexcept
        : snapshot_(std::move(snapshot)), appearance_(appearance) {}

    /** @copydoc CaptionSettingsSnapshot::Appearance */
    const CaptionAppearance &CaptionSettingsSnapshot::Appearance() const noexcept {
        return appearance_;
    }

    /** @copydoc CaptionSettingsSnapshot::Revision */
    ConfigurationRevision CaptionSettingsSnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc CaptionSettingsSnapshot::Configuration */
    const ConfigurationSnapshot &CaptionSettingsSnapshot::Configuration() const noexcept {
        return snapshot_;
    }
}  // namespace Horo::Runtime::Ui
