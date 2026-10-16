#pragma once

/** @file CharacterPolicies.h
 * @brief Fixed Character movement policies shared by durable scene values and live controller contracts.
 */

#include <cstdint>

namespace Horo::Character {
    inline constexpr std::uint32_t MaximumCharacterContacts = 32;       /**< Fixed admitted contact bound. */
    inline constexpr double CharacterUnitSquaredNormTolerance = 1.0e-6; /**< Authored and resident unit-basis tolerance. */
    /** @brief Deliberate response to a non-walkable upward-facing surface. */
    enum class CharacterSteepSlopePolicy : std::uint8_t {
        Stop,  /**< Blocks uphill intent and applies no automatic surface drift. */
        Slide, /**< Integrates committed Character gravity velocity along the steep surface. */
    };

    /** @brief Velocity transferred once when a valid moving support is left. */
    enum class CharacterPlatformLeaveVelocityPolicy : std::uint8_t {
        None,   /**< No support velocity enters free-flight continuation. */
        Linear, /**< Transfer only the supporting body's center-of-mass linear velocity. */
        Point,  /**< Transfer linear velocity plus angular velocity at the attachment point. */
    };

}  // namespace Horo::Character
