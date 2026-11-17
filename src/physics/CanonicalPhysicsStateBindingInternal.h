#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateBinding.h"

namespace Horo::Physics::Detail {
    [[nodiscard]] Result<void> BindCanonicalStateBodies(const CanonicalWorld &world, std::span<const PhysicsStateBodyBinding> bindings,
                                                        CanonicalPhysicsStateBindingStorage &storage);
    [[nodiscard]] Result<void> BindCanonicalStateJoints(const CanonicalWorld &world,
                                                        std::span<const PhysicsStateConstraintBinding> bindings,
                                                        CanonicalPhysicsStateBindingStorage &storage);
}  // namespace Horo::Physics::Detail
