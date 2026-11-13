#pragma once

#include "CanonicalPhysicsStateBinding.h"
#include "CanonicalPhysicsStateFields.h"

namespace Horo::Physics::Detail {
    /** @brief Produces a private native restore bridge from fully admitted typed
     * state and exact retained topology. */
    [[nodiscard]] Result<void> EncodeCanonicalPhysicsState(CanonicalPhysicsStateStream &stream,
                                                           const CanonicalPhysicsStateBindings &bindings,
                                                           const PhysicsStateSolverView &state);
    /** @brief Encodes complete native warm-start contact/CCD sections using real
     * rebound native identities. */
    [[nodiscard]] Result<void> EncodeCanonicalPhysicsContacts(CanonicalPhysicsStateStream &stream,
                                                              const CanonicalPhysicsStateBindings &bindings,
                                                              const PhysicsStateSolverView &state);
}  // namespace Horo::Physics::Detail
