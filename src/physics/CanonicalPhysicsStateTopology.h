#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateBinding.h"

namespace Horo::Physics::Detail {
    /** @brief Checks every snapshot identity/subshape against exact retained native
     * shape and body topology before reconstruction. */
    [[nodiscard]] Result<void> ValidateCanonicalPhysicsStateTopology(const CanonicalWorld &world,
                                                                     const CanonicalPhysicsStateBindings &bindings,
                                                                     const PhysicsStateSolverView &state,
                                                                     const PhysicsWorldSettings &settings);
    /** @brief Prepares an isolated solver under the same runtime and PhysicsWorld
     * transaction; never publishes a world identity. */
    [[nodiscard]] Result<CanonicalWorldHandle> CloneCanonicalPhysicsStateTopology(CanonicalWorldHandle source,
                                                                                  const PhysicsWorldSettings &settings);
}  // namespace Horo::Physics::Detail
