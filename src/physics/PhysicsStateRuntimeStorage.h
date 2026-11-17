#pragma once
#include "PhysicsStateConstructionStorage.h"
#include "PhysicsStateRuntimeInventory.h"

namespace Horo::Physics::Detail {
    struct PreparedPhysicsStateRuntimeStorage final {
        std::shared_ptr<StateDetail::ConstructionRecipePool> recipes;
        std::unique_ptr<PhysicsStateRuntimeInventory> inventory;
    };

    /** @brief Exception-capable cold admission of actual fixed recipe data under the SAME aggregate Runtime budget. */
    [[nodiscard]] Result<PreparedPhysicsStateRuntimeStorage> PreparePhysicsStateRuntimeStorage(const PhysicsStateRuntimeBudget &);
}  // namespace Horo::Physics::Detail
