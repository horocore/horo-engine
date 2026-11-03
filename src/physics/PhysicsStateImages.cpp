#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateImagesInternal.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>

namespace Horo::Physics::Detail {
    /** @copydoc ReadPhysicsStateImageAddresses */
    PhysicsStateImageAddresses ReadPhysicsStateImageAddresses() noexcept {
        return {reinterpret_cast<const void *>(&StepCanonicalWorld), reinterpret_cast<const void *>(&JPH::RegisterTypes)};
    }
}  // namespace Horo::Physics::Detail
