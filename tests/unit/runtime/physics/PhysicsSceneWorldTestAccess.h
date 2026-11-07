#pragma once

#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

namespace Horo::Physics {
    /** @brief Borrows the aggregate world solely for deterministic containment regression coverage. */
    struct PhysicsSceneContainmentTestAccess final {
        static PhysicsWorld &World(PhysicsSceneActivationCandidate &candidate) {
            return *candidate.physics_;
        }

        /** @brief Installs a genuinely prepared/activated private peer for negative owner-history tests.
         * The caller uses public lifecycle operations; no state, controller generation or tick is fabricated.
         */
        static void ReplaceCharacter(PhysicsSceneActivationCandidate &candidate, std::unique_ptr<Character::CharacterWorld> peer) {
            candidate.character_->Shutdown();
            candidate.character_ = std::move(peer);
        }

        static Character::CharacterWorld &CharacterWorld(PhysicsSceneActivationCandidate &candidate) {
            return *candidate.character_;
        }
    };

}  // namespace Horo::Physics
