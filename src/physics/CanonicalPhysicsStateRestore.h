#pragma once

#include "CanonicalPhysicsStateCapture.h"

namespace Horo::Physics::Detail {
    /** @brief Complete private restore scratch is cold prepared; recapture storage
     * never aliases submitted immutable snapshot values. */
    struct CanonicalPhysicsStateRestoreScratch final {
        CanonicalPhysicsStateCaptureScratch native;
        std::span<std::byte> expectedWire;
        std::span<std::byte> actualWire;
        StateDetail::SolverStorage &recapture;
    };

    /** @brief Restores ONLY an already cold-prepared unpublished native candidate,
     * retaining every source identity and lease.
     * @details Input grammar/topology is fully validated first. Exact canonical
     * recapture must match before a native handle is returned. Failure leaves only
     * unpublished candidate values changed; the caller owns both native worlds.
     * Subsequent attempts overwrite its complete solver state. Native
     * topology/capacity allocation is never a restore operation. The enclosing
     * PhysicsWorld candidate retains runtime/code/content leases and publishes at
     * its existing owner fence.
     */
    [[nodiscard]] Result<CanonicalWorldHandle> PrepareCanonicalPhysicsStateRestore(
        CanonicalWorldHandle source, CanonicalWorldHandle candidate, const PhysicsWorldSettings &settings,
        const CanonicalPhysicsStateBindings &bindings, const PhysicsStateSolverView &state,
        const PhysicsContinuousCollisionPolicy &continuousCollision, CanonicalPhysicsStateRestoreScratch scratch);
}  // namespace Horo::Physics::Detail
