#pragma once
#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <vector>

namespace Horo::Physics::Detail {
    /** @brief Receiving-owner routing only. IDs are canonical full-key projection; sceneEntity is reissued locally, never read from wire.
     */
    struct CanonicalRestartBodyRoute final {
        PhysicsStateObjectId body{};
        std::uint64_t sceneEntity{};
    };

    /** @brief Actual newly issued handles under the same unpublished receiving World. No serialized native/local handle is admitted. */
    struct CanonicalRestartBindings final {
        std::vector<PhysicsStateShapeBinding> shapes;
        std::vector<PhysicsStateBodyBinding> bodies;
        std::vector<PhysicsStateConstraintBinding> constraints;
    };

}  // namespace Horo::Physics::Detail
