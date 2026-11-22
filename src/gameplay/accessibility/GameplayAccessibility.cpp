#include "Horo/Gameplay/GameplayAccessibility.h"

namespace Horo::Gameplay {
    namespace {
        /** @brief One numeric preference's key, neutral value and authoritative limits. */
        struct NumericAssist {
            SettingKey key;
            std::int64_t neutral;
            SettingIntegerRange range;
        };

        /** @brief Inert gameplay configuration metadata shared by schema and tick capture. */
        struct AssistSettings {
            NumericAssist aim{SettingKey{"accessibility.gameplay.aim_assist"}, 0, {0, 1000}};
            SettingKey snap{"accessibility.gameplay.auto_aim_snap"};
            NumericAssist aggression{SettingKey{"accessibility.gameplay.aggression_reduction"}, 0, {0, 1000}};
            NumericAssist reaction{SettingKey{"accessibility.gameplay.reaction_time_multiplier"}, 1000, {1000, 10000}};
            SettingKey skipQte{"accessibility.gameplay.skip_qte"};
            NumericAssist invulnerability{SettingKey{"accessibility.gameplay.post_hit_invulnerability_ms"}, 0, {0, 60000}};
            NumericAssist damage{SettingKey{"accessibility.gameplay.damage_multiplier"}, 1000, {0, 10000}};
        };

        /** @brief Initializes key storage on first use without registering settings. */
        const AssistSettings &Settings() {
            static const AssistSettings settings;
            return settings;
        }

        /** @brief Builds a user preference descriptor with the gameplay source policy. */
        SettingDescriptor Describe(const SettingKey &key, const SettingValueType type, SettingValue value,
                                   SettingValidation validation = {}) {
            return {.key = key,
                    .type = type,
                    .defaultValue = std::move(value),
                    .scope = SettingScope::User,
                    .reloadPolicy = ReloadPolicy::NextFrame,
                    .sensitivity = SettingSensitivity::Public,
                    .sourcePolicy =
                        ConfigurationSourcePolicy{ConfigurationSourceMask::User | ConfigurationSourceMask::Session |
                                                      ConfigurationSourceMask::Invocation | ConfigurationSourceMask::PackagedProfile,
                                                  false},
                    .validation = std::move(validation)};
        }

        /** @brief Publishes the bounds used by fixed tick capture to configuration ingress. */
        SettingDescriptor Describe(const NumericAssist &assist) {
            return Describe(assist.key, SettingValueType::Integer, assist.neutral, assist.range);
        }

        /** @brief Reads one exact typed domain value without allocation or fallback to another settings store. */
        template <typename T> const T *Value(const ConfigurationSnapshot &snapshot, const SettingKey &key) noexcept {
            const auto *entry = snapshot.FindResolved(key);
            return entry ? std::get_if<T>(&entry->value) : nullptr;
        }

        /** @brief Checks snapshots supplied by hosts that may have composed their own schema. */
        const std::int64_t *BoundedValue(const ConfigurationSnapshot &snapshot, const NumericAssist &assist) noexcept {
            const auto *value = Value<std::int64_t>(snapshot, assist.key);
            return value && *value >= assist.range.minimum && *value <= assist.range.maximum ? value : nullptr;
        }
    }  // namespace

    /** @copydoc MakeGameplayAccessibilityContribution */
    ModuleConfigurationContribution MakeGameplayAccessibilityContribution() {
        const auto &settings = Settings();
        return {.module = ModuleId{"horo.gameplay.accessibility"},
                .ownerPrefix = "accessibility.gameplay",
                .settings = {Describe(settings.aim), Describe(settings.snap, SettingValueType::Boolean, false),
                             Describe(settings.aggression), Describe(settings.reaction),
                             Describe(settings.skipQte, SettingValueType::Boolean, false), Describe(settings.invulnerability),
                             Describe(settings.damage)}};
    }

    /** @copydoc GameplayAssistTickSnapshot::Capture */
    Result<GameplayAssistTickSnapshot> GameplayAssistTickSnapshot::Capture(ConfigurationSnapshot snapshot) {
        const auto &settings = Settings();
        const auto *aim = BoundedValue(snapshot, settings.aim);
        const auto *snap = Value<bool>(snapshot, settings.snap);
        const auto *aggression = BoundedValue(snapshot, settings.aggression);
        const auto *reaction = BoundedValue(snapshot, settings.reaction);
        const auto *qte = Value<bool>(snapshot, settings.skipQte);
        const auto *invulnerability = BoundedValue(snapshot, settings.invulnerability);
        const auto *damage = BoundedValue(snapshot, settings.damage);
        if (!aim || !snap || !aggression || !reaction || !qte || !invulnerability || !damage)
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
