#include "CanonicalPhysicsStateBackendInternal.h"
#include "CanonicalPhysicsStateIntent.h"

namespace Horo::Physics::Detail {
    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateState */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateState(const PhysicsStateSolverView &state,
                                                               const PhysicsContinuousCollisionPolicy &continuousCollision) {
        if (!impl_->owner.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (auto policy = ValidateCanonicalContinuousCollision(impl_->owner, continuousCollision); policy.HasError())
            return policy;
        if (auto typed = ValidatePhysicsStateSolver(state, impl_->recapture->limits, impl_->recapture->activeBodyScratch); typed.HasError())
            return typed;
        const auto &world = *static_cast<const CanonicalWorld *>(impl_->owner.value);
        if (auto topology = ValidateCanonicalPhysicsStateTopology(world, impl_->bindings.View(), state, impl_->settings);
            topology.HasError())
            return topology;
        return ValidateCanonicalStateIntent(world, impl_->bindings.View(), state);
    }
}  // namespace Horo::Physics::Detail
