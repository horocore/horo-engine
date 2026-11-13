#include "Horo/Gameplay/GameplayAccessibility.h"

#include <array>

namespace Horo::Gameplay {
    namespace {
        /** @brief Inert domain key metadata, initialized during explicit contribution composition. */
        const std::array<SettingKey, 7> &Keys() {
            static const std::array<SettingKey, 7> keys{SettingKey{"accessibility.gameplay.aim_assist"},
                                                        SettingKey{"accessibility.gameplay.auto_aim_snap"},
                                                        SettingKey{"accessibility.gameplay.aggression_reduction"},
                                                        SettingKey{"accessibility.gameplay.reaction_time_multiplier"},
                                                        SettingKey{"accessibility.gameplay.skip_qte"},
                                                        SettingKey{"accessibility.gameplay.post_hit_invulnerability_ms"},
                                                        SettingKey{"accessibility.gameplay.damage_multiplier"}};
            return keys;
        }

        /** @brief Reads one exact typed domain value without allocation or fallback to another settings store. */
        template <typename T> const T *Value(const ConfigurationSnapshot &snapshot, const std::size_t index) noexcept {
            const auto *entry = snapshot.FindResolved(Keys()[index]);
            return entry ? std::get_if<T>(&entry->value) : nullptr;
        }
    }  // namespace

    /** @copydoc MakeGameplayAccessibilityContribution */
    ModuleConfigurationContribution MakeGameplayAccessibilityContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.gameplay.accessibility"},
                                                     .ownerPrefix = "accessibility.gameplay"};
        const auto &keys = Keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const bool boolean = i == 1 || i == 4;
            SettingValue value = std::int64_t{0};
            if (boolean)
                value = false;
            else if (i == 3 || i == 6)
                value = std::int64_t{1000};
            contribution.settings.push_back(
                {keys[i], boolean ? SettingValueType::Boolean : SettingValueType::Integer, std::move(value), SettingScope::User,
                 ReloadPolicy::NextFrame, SettingSensitivity::Public,
                 ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                           false}});
        }
        return contribution;
    }

    /** @copydoc GameplayAssistTickSnapshot::Capture */
    Result<GameplayAssistTickSnapshot> GameplayAssistTickSnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto *aim = Value<std::int64_t>(snapshot, 0);
        const auto *snap = Value<bool>(snapshot, 1);
        const auto *aggression = Value<std::int64_t>(snapshot, 2);
        const auto *reaction = Value<std::int64_t>(snapshot, 3);
        const auto *qte = Value<bool>(snapshot, 4);
        const auto *invulnerability = Value<std::int64_t>(snapshot, 5);
        const auto *damage = Value<std::int64_t>(snapshot, 6);
        if (!aim || !snap || !aggression || !reaction || !qte || !invulnerability || !damage || *aim < 0 || *aim > 1000 ||
            *aggression < 0 || *aggression > 1000 || *reaction < 1000 || *reaction > 10000 || *invulnerability < 0 ||
            *invulnerability > 60000 || *damage < 0 || *damage > 10000)
            return Result<GameplayAssistTickSnapshot>::Failure(MakeError(GameplayAccessibilityErrors::InvalidSettings));
        GameplayAssistPreferences preferences{static_cast<float>(*aim) / 1000,      *snap, static_cast<float>(*aggression) / 1000,
                                              static_cast<float>(*reaction) / 1000, *qte,  static_cast<float>(*invulnerability) / 1000,
                                              static_cast<float>(*damage) / 1000};
        return Result<GameplayAssistTickSnapshot>::Success(GameplayAssistTickSnapshot{std::move(snapshot), preferences});
    }

    /** @copydoc GameplayAssistTickSnapshot::GameplayAssistTickSnapshot */
    GameplayAssistTickSnapshot::GameplayAssistTickSnapshot(ConfigurationSnapshot snapshot,
                                                           const GameplayAssistPreferences preferences) noexcept
        : snapshot_(std::move(snapshot)), preferences_(preferences) {}

    /** @copydoc GameplayAssistTickSnapshot::Preferences */
    const GameplayAssistPreferences &GameplayAssistTickSnapshot::Preferences() const noexcept {
        return preferences_;
    }

    /** @copydoc GameplayAssistTickSnapshot::Revision */
    ConfigurationRevision GameplayAssistTickSnapshot::Revision() const noexcept {
        return snapshot_.Revision();
    }

    /** @copydoc GameplayAssistTickSnapshot::Configuration */
    const ConfigurationSnapshot &GameplayAssistTickSnapshot::Configuration() const noexcept {
        return snapshot_;
    }

    /** @copydoc GameplayAssistSettingsAdapter::GameplayAssistSettingsAdapter */
    GameplayAssistSettingsAdapter::GameplayAssistSettingsAdapter(EngineDataBus &bus) noexcept : bus_(bus) {}

    /** @copydoc GameplayAssistSettingsAdapter::NotifyCommitted */
    void GameplayAssistSettingsAdapter::NotifyCommitted(const ConfigurationSnapshot &snapshot) {
        const auto revision = snapshot.Revision();
        if (revision <= lastHint_)
            return;
        bus_.PublishAsync(GameplayAccessibilityStateEvent{revision});
        lastHint_ = revision;
    }

    namespace GameplayAccessibilityErrors {
        /** @copydoc InvalidSettings */
        const ErrorCodeDescriptor InvalidSettings{ErrorDomainId{"horo.gameplay.accessibility"},
                                                  ErrorCode{"gameplay.accessibility.settings_invalid"},
                                                  ErrorSeverity::Error,
                                                  "Gameplay accessibility preferences are invalid.",
                                                  "Compose the domain settings and use valid bounded values.",
                                                  false,
                                                  false};
    }  // namespace GameplayAccessibilityErrors
}  // namespace Horo::Gameplay
