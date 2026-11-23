#pragma once
#include "Horo/Physics/PhysicsConstraintDescriptor.h"

namespace Horo::Physics::Detail {
    /** @brief Same canonical parameter/collision admission for normal runtime creation and durable construction recipes.
     * Does not validate or create endpoints, native resources, or capabilities.
     */
    [[nodiscard]] Result<void> ValidateCanonicalConstraintParameters(const decltype(PhysicsConstraintDescriptor::parameters) &,
                                                                     PhysicsJointCollisionPolicy);
}  // namespace Horo::Physics::Detail
