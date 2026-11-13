#pragma once
#include "Horo/Physics/PhysicsStateSnapshot.h"
#include "Horo/Physics/PhysicsWorld.h"

namespace Horo::Application::Internal {
    /** @brief Binds actual process-static Physics code identity before any world or state lease admission.
     * @param runtime Actual selected process Physics owner on its creating thread.
     * @param budget Explicit cold aggregate owner-record/payload limits supplied by application composition.
     * @return Original image/native admission failure; never selects a backend or substitutes identity evidence.
     * @pre Canonical Physics and Jolt are linked into the actual executable; a dynamic module needs its real loader lease.
     */
    [[nodiscard]] Result<void> BindProcessStaticPhysicsState(Physics::PhysicsRuntime &runtime, Physics::PhysicsStateRuntimeBudget budget);
}  // namespace Horo::Application::Internal
