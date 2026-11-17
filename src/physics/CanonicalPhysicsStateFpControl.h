#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"

namespace Horo::Physics::Detail {
    /** @brief Reads actual current thread control bits without changing host FP
     * state or claiming qualification.
     * @details Status/sticky exception flags are excluded; they are diagnostics,
     * not inputs to Physics simulation. Restore/capture must match the immutable
     * bound owner control. Unknown native targets fail closed.
     */
    [[nodiscard]] Result<PhysicsStateFpControl> ReadCanonicalPhysicsStateFpControl();
}  // namespace Horo::Physics::Detail
