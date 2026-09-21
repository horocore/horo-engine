#pragma once

#include "Horo/Physics/PhysicsSceneActivation.h"

namespace Horo::Physics::Detail {
    /** @brief Verifies the capabilities required by the active authored physics portions of a scene. */
    [[nodiscard]] Result<void> RequirePhysicsSceneCapabilities(const PhysicsRuntime &runtime,
                                                               const Runtime::RuntimeSceneDefinition &definition);
}  // namespace Horo::Physics::Detail
