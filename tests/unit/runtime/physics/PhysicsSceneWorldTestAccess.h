#pragma once

#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

namespace Horo::Physics {
    /** @brief Borrows the aggregate world solely for deterministic containment regression coverage. */
    struct PhysicsSceneContainmentTestAccess final {
        static PhysicsWorld &World(PhysicsSceneActivationCandidate &candidate) {
            return *candidate.physics_;
        }

        static Character::CharacterWorld &CharacterWorld(PhysicsSceneActivationCandidate &candidate) {
            return *candidate.character_;
        }
    };

}  // namespace Horo::Physics
