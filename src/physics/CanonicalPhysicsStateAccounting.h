#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Actual Horo-side capacities around one native owner, excluding native
     * allocations observed by the installed hooks. */
    [[nodiscard]] Result<std::size_t> CanonicalPhysicsStateOwnerPayloadBytes(const CanonicalWorld &world);
}  // namespace Horo::Physics::Detail
