#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

namespace Horo::Physics {
    TEST_CASE("Null Physics state operations fail typed without a native world, build fact fallback, or fabricated snapshot",
              "[physics][state][omitted]") {
        auto runtimeResult = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
        REQUIRE(runtimeResult.HasValue());
        auto runtime = std::move(runtimeResult).Value();
        Test::RequireError(runtime->BindStateSnapshotBuild({}, {1, 4096}), PhysicsErrors::OperationUnsupported);
        auto worldResult = runtime->PrepareWorld(Test::SmallWorldSettings());
        REQUIRE(worldResult.HasValue());
        auto world = std::move(worldResult).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(400).Value()).HasValue());
        Test::RequireError(world->ConfigureStateSnapshots({}), PhysicsErrors::OperationUnsupported);
        Test::RequireError(world->CaptureState(), PhysicsErrors::OperationUnsupported);
        Test::RequireError(world->DecodeStateSnapshot({}), PhysicsErrors::OperationUnsupported);
        Test::RequireError(world->PrepareStateRestore({}), PhysicsErrors::OperationUnsupported);
    }
}  // namespace Horo::Physics
