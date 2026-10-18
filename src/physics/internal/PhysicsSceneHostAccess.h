#pragma once

/** @file PhysicsSceneHostAccess.h
 * @brief Non-installed application-owner access to the one aggregate-owned published Physics/Character pair.
 */
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

namespace Horo::Physics::Detail {
    /** @brief Synchronous owner-lane borrow; carries no ownership or permission to survive candidate retirement.
     * @details The application owns the Scene service and clears its published candidate observer before Shutdown.
     * Never retain this value in a callback, job, gameplay module, or beyond the current owner-lane operation.
     */
    struct PhysicsSceneHostScope final {
        PhysicsWorld &physics;
        Character::CharacterWorld &character;
        Runtime::SceneRuntimeId scene;
    };

    /** @brief Verifies publication and exact paired lifetime without exporting world ownership to the SDK. */
    class PhysicsSceneHostAccess final {
    public:
        /** @brief Borrows the published aggregate pair for host tick dispatch or explicit capability composition.
         * @param candidate Borrowed solely from the application's live aggregate observer.
         * @return Paired owner references or the original affinity/evidence/lifecycle failure; unpublished candidates reject.
         * @pre The owner thread serializes access with replacement/rollback/close; the Scene service remains sole candidate owner.
         */
        [[nodiscard]] static Result<PhysicsSceneHostScope> Borrow(PhysicsSceneActivationCandidate &candidate);
    };
}  // namespace Horo::Physics::Detail
