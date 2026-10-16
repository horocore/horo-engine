#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "navigation/NavigationTestAssertions.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    TEST_CASE("Runtime qualification records actual native provider admission without silent substitution",
              "[integration][navigation][qualification][availability]") {
#if HORO_QUALIFICATION_NAV_NATIVE
        REQUIRE(CreateRecastDetourCrowdBackend().HasValue());
#else
        TestSupport::RequireError(CreateRecastDetourCrowdBackend(), NavigationErrors::OperationUnsupported);
#endif
#if HORO_QUALIFICATION_PHYSICS_NATIVE
        REQUIRE(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).HasValue());
#else
        TestSupport::RequireError(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical),
                                  Physics::PhysicsErrors::CapabilityUnavailable);
#endif
    }
}  // namespace Horo::Navigation
