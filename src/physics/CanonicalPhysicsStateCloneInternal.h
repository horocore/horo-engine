#pragma once

#include "CanonicalPhysicsStateTopology.h"

namespace Horo::Physics::Detail {
    /** @brief Recreates every exact body ID using retained immutable shapes without
     * touching the source solver. */
    [[nodiscard]] Result<void> CloneCanonicalStateBodies(const CanonicalWorld &source, CanonicalWorld &candidate);
    /** @brief Recreates native manager order from real constraints, retaining
     * complete Horo descriptors and endpoints. */
    [[nodiscard]] Result<void> CloneCanonicalStateJoints(const CanonicalWorld &source, CanonicalWorld &candidate);
}  // namespace Horo::Physics::Detail
