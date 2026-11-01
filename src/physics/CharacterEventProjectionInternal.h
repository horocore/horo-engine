#pragma once
#include "Horo/Physics/CharacterEvents.h"

namespace Horo::Character::Detail {
    /** @brief Prior committed facts only; candidate events and raw callback order are never transition history. */
    struct CharacterEventPreviousState final {
        bool grounded{}, sliding{};
        CharacterCollisionFlags collisions{CharacterCollisionFlags::None};
        std::optional<Physics::PhysicsSurfaceEvidence> groundSurface;
    };

    /** @brief At most one fact of each kind can be produced by one final committed controller command. */
    struct CharacterEventProjection final {
        std::array<CharacterMovementEvent, 7> events;
        std::uint8_t count{};
    };

    [[nodiscard]] CharacterEventProjection ProjectCharacterMovementEvents(const CharacterEventPreviousState &previous,
                                                                          const CharacterLocomotionSnapshot &current,
                                                                          const CharacterControllerDescriptor &descriptor) noexcept;
    [[nodiscard]] CharacterEventTerminalState ProjectCharacterTerminalState(const CharacterLocomotionSnapshot &current) noexcept;
    [[nodiscard]] bool CharacterMovementEventLess(const CharacterMovementEvent &left, const CharacterMovementEvent &right) noexcept;
    [[nodiscard]] bool CharacterMovementEventRetainBefore(const CharacterMovementEvent &left, const CharacterMovementEvent &right) noexcept;
}  // namespace Horo::Character::Detail
