#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateProcessBinding.h"

namespace Horo::Application::Internal {
    /** @copydoc BindProcessStaticPhysicsState */
    Result<void> BindProcessStaticPhysicsState(Physics::PhysicsRuntime &, Physics::PhysicsStateRuntimeBudget) {
        return Result<void>::Failure(
            MakeError(Physics::PhysicsErrors::OperationUnsupported, "Canonical Physics state image binding is unavailable in this build."));
    }
}  // namespace Horo::Application::Internal
