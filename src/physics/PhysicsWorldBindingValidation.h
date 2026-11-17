#pragma once
#include "Horo/Physics/PhysicsWorld.h"

namespace Horo::Physics::Detail {
    /** @brief Share complete immutable simulation admission between world composition and private native preparation. */
    inline Result<void> ValidateWorldSimulationBinding(const PhysicsWorldSimulationBinding &binding) {
        if (static_cast<bool>(binding.schema) != (binding.generation != 0))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Collision schema owner and generation must be supplied together."));
        return ValidatePhysicsSurfaceWorldBinding(binding.surfaces);
    }
}  // namespace Horo::Physics::Detail
