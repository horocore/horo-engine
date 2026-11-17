#include "Horo/Physics/CharacterWorld.h"
#include "PhysicsSceneStateTarget.h"
#include "internal/CharacterScenePlacement.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsSceneStateTarget::InstallBindings */
    void PhysicsSceneStateTarget::InstallBindings(PhysicsStructuralState &state, PhysicsSceneStateRebind &tables) noexcept {
        state.bodies.swap(tables.bodies);
        state.shapes.swap(tables.shapes);
        state.constraints.swap(tables.constraints);
        state.authoredBodies.swap(tables.authoredBodies);
        state.authoredShapes.swap(tables.authoredShapes);
        state.authoredConstraints.swap(tables.authoredConstraints);
        candidate_->bodyBindings_.swap(tables.legacyBodies);
        candidate_->shapeBindings_.swap(tables.legacyShapes);
        candidate_->constraintBindings_.swap(tables.legacyConstraints);
        ++state.revision;  // Same existing structural publication revision; capacity checked before reconstruction.
        revision_ = state.revision;
    }

    /** @copydoc PhysicsSceneStateTarget::RefreshEmptyCharacter */
    Result<void> PhysicsSceneStateTarget::RefreshEmptyCharacter() const {
        return Character::Detail::CharacterScenePlacement::AdoptEmptyPendingContinuation(*candidate_->character_, *candidate_->physics_,
                                                                                         scene_);
    }

    /** @copydoc PhysicsSceneStateTarget::PreflightPendingContinuation */
    Result<void> PhysicsSceneStateTarget::PreflightPendingContinuation() const {
        return Character::Detail::CharacterScenePlacement::ValidateEmptyPendingContinuation(*candidate_->character_, *candidate_->physics_,
                                                                                            scene_);
    }
}  // namespace Horo::Physics::Detail
