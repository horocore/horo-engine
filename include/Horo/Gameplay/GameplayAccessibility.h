#pragma once

/** @file GameplayAccessibility.h
 * @brief Gameplay-owned snapshot preferences and coarse revision hints; no simulation authority is granted.
 */
#include "Horo/Foundation/ModuleHost.h"

namespace Horo::Gameplay {
    /** @brief Desired preferences only; each gameplay consumer still applies its normal capability/authority rules. */
    struct GameplayAssistPreferences final {
        float aimAssist{};
        bool autoAimSnap{};
        float aggressionReduction{};
        float reactionTimeMultiplier{1};
        bool skipQuickTimeEvents{};
        float postHitInvulnerabilitySeconds{};
        float damageMultiplier{1};
    };

    /** @brief Sole approved accessibility bus payload: a hint, never authoritative assist values. */
    struct GameplayAccessibilityStateEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo::GameplayAccessibilityStateEvent";
        ConfigurationRevision revision{};
    };

    /**
     * @brief Immutable authoritative preference view for a complete simulation tick.
     * @details At EVERY tick start the host captures ConfigurationService::Snapshot and calls
     * Capture, regardless of hint delivery. All consumers in that tick share this value. No bus
     * subscriber mutates it. Unsupported/unauthorized assists remain unapplied by the gameplay
     * owner; reading this view is not a command permission, server grant or cheat bypass.
     */
    class GameplayAssistTickSnapshot final {
    public:
        /** @brief Validates and captures one complete authoritative revision.
         * @param snapshot Tick-start snapshot with the gameplay contribution registered.
         * @return Coherent typed view or GameplayAccessibilityErrors::InvalidSettings.
         * @details Strengths and multipliers use integer thousandths in configuration; the
         * invulnerability window uses integer milliseconds. This is the sole wire encoding.
         */
        [[nodiscard]] static Result<GameplayAssistTickSnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @brief Returns immutable desired preferences. @return Tick-stable values. */
        [[nodiscard]] const GameplayAssistPreferences &Preferences() const noexcept;
        /** @brief Returns the exact authority revision. @return Captured revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;
        /** @brief Borrows configuration provenance retained for the tick. @return Immutable authority lease. */
        [[nodiscard]] const ConfigurationSnapshot &Configuration() const noexcept;

    private:
        GameplayAssistTickSnapshot(ConfigurationSnapshot snapshot, GameplayAssistPreferences preferences) noexcept;
        ConfigurationSnapshot snapshot_;
        GameplayAssistPreferences preferences_;
    };

    /** @brief Host-composed, owner-thread hint publisher; has no subscriptions, gameplay callbacks or settings store. */
    class GameplayAssistSettingsAdapter final {
    public:
        /** @brief Borrows the process bus, which must outlive this adapter. @param bus Application-owned bus. */
        explicit GameplayAssistSettingsAdapter(EngineDataBus &bus) noexcept;
        /** @brief Queues at most one hint per new monotonic committed revision outside the simulation tick.
         * @param snapshot Already committed authoritative snapshot; stale/duplicate revisions are ignored.
         * @throws std::bad_alloc When bus payload reservation fails; a retry remains eligible.
         * @details Notification loss is allowed; tick capture must never depend on this method succeeding.
         */
        void NotifyCommitted(const ConfigurationSnapshot &snapshot);

    private:
        EngineDataBus &bus_;
        ConfigurationRevision lastHint_{};
    };

    /** @brief Returns inert User/NextFrame descriptors exclusively for accessibility.gameplay.*.
     * @return Domain-owned contribution; no registration, bus publication or service discovery occurs.
     * @details Aim/aggression range [0,1000]; reaction [1000,10000]; damage [0,10000];
     * invulnerability [0,60000]ms. Defaults are neutral; Capture enforces all domain ranges.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeGameplayAccessibilityContribution();

    namespace GameplayAccessibilityErrors {
        /** @brief The authoritative snapshot lacks a required key, has the wrong type, or violates an assist range. */
        extern const ErrorCodeDescriptor InvalidSettings;
    }  // namespace GameplayAccessibilityErrors
}  // namespace Horo::Gameplay
