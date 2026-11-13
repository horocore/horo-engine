#include "CanonicalPhysicsStateBindingInternal.h"

namespace Horo::Physics::Detail {
    /** @copydoc PrepareCanonicalPhysicsStateBindings */
    Result<CanonicalPhysicsStateBindingStorage> PrepareCanonicalPhysicsStateBindings(
        const CanonicalWorld &world, std::span<const PhysicsStateBodyBinding> bodies,
        std::span<const PhysicsStateConstraintBinding> constraints) {
        using Outcome = Result<CanonicalPhysicsStateBindingStorage>;
        if (!world.native.system || !world.pendingBodyBatch.expired() || world.contactRoute.load())
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        CanonicalPhysicsStateBindingStorage result;
        if (auto body = BindCanonicalStateBodies(world, bodies, result); body.HasError())
            return Outcome::Failure(body.ErrorValue());
        if (auto joint = BindCanonicalStateJoints(world, constraints, result); joint.HasError())
            return Outcome::Failure(joint.ErrorValue());
        return Outcome::Success(std::move(result));
    }
}  // namespace Horo::Physics::Detail
