#pragma once

/** @file GameplayAssistPreferences.h @brief Desired player preferences offered to fixed-step gameplay callbacks. */

namespace Horo::Gameplay {
    /** @brief Tick-stable desired settings. Each game still authorizes and implements the assists it supports. */
    struct GameplayAssistPreferences final {
        float aimAssist{};
        bool autoAimSnap{};
        float aggressionReduction{};
        float reactionTimeMultiplier{1};
        bool skipQuickTimeEvents{};
        float postHitInvulnerabilitySeconds{};
        float damageMultiplier{1};
    };
}  // namespace Horo::Gameplay
