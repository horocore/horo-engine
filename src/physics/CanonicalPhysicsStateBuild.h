#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"

namespace Horo::Physics::Detail {
    /** @brief Runtime-owned exact native build tuple; payload contains no published
     * capability or replacement clock. */
    struct CanonicalPhysicsStateBuildIdentity final {
        Sha256Digest build, solver, platform;
        PhysicsStateFpControl floatingPoint;
    };

    /** @brief Combines genuine host image evidence with exact generated native
     * facts; unknown facts have no fallback. */
    [[nodiscard]] Result<CanonicalPhysicsStateBuildIdentity> PrepareCanonicalPhysicsStateBuild(const PhysicsStateBuildBinding &binding);
}  // namespace Horo::Physics::Detail
