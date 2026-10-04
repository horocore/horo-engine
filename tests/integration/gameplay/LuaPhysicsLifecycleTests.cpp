#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>

TEST_CASE("Lua execution without an injected Physics holder returns stable unavailable diagnostics", "[gameplay-physics][lua]") {
    using namespace Horo;
    using namespace Horo::Gameplay;
    const auto type = BehaviorTypeId::Parse("game.tests.physics_unavailable").Value();
    auto program = LuaBehaviorProgram::Compile(R"(
return horo.behavior {
    type_id = "game.tests.physics_unavailable", display_name = "No Physics host",
    on_create = function(ctx)
        local capability, error = ctx.physics.acquire()
        assert(capability == nil and error.code == "gameplay.physics.unavailable")
    end
}
)",
                                               type, "physics_unavailable.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
    REQUIRE(registry.Freeze().HasValue());
    auto active = Tests::ActivateBehaviorRuntime(Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type), {7}, registry);
    REQUIRE(active.runtime->FixedUpdate({}, {1.0 / 60.0}).HasValue());
}

#if HORO_TEST_PHYSICS_NATIVE
TEST_CASE("Lua Physics preserves copied hits and terminal batches across world retirement", "[gameplay-physics][lua][lifetime]") {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;
    auto physics = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto world = physics->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    REQUIRE(world->Activate(physics->IssueWorldIdentity().Value()).HasValue());
    REQUIRE(world
                ->CreateQueryFixture({.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                      .pose = {.translation = {0, 0, -5}, .rotation = Math::Quaternion::Identity()},
                                      .layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                      .profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value(),
                                      .channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value()})
                .HasValue());
    REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                .HasValue());
    auto context = GameplayPhysicsContext::Create({"game.tests", 7, 1, world->Identity(), true, true}, world.get()).Value();
    const auto type = BehaviorTypeId::Parse("game.tests.physics_lifecycle").Value();
    std::ifstream stream{std::filesystem::path{HORO_TEST_SOURCE_ROOT} / "tests/fixtures/gameplay_physics/PhysicsLifecycle.horo_script"};
    REQUIRE(stream.good());
    std::array<char, 16 * 1024> script{};
    stream.read(script.data(), static_cast<std::streamsize>(script.size()));
    REQUIRE(stream.eof());
    REQUIRE_FALSE(stream.bad());
    REQUIRE(stream.gcount() > 0);
    const std::string source = "local revision = " + std::to_string(world->PublishedTick().publicationRevision) + "\n" +
                               std::string{script.data(), static_cast<std::size_t>(stream.gcount())};
    auto program = LuaBehaviorProgram::Compile(source, type, "physics_lifecycle.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
    REQUIRE(registry.Freeze().HasValue());
    auto definition = Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type);
    auto scene = Runtime::RuntimeScene::Create(definition, {7}).Value();
    auto behavior = BehaviorRuntime::Create(*scene, registry, {}, context);
    REQUIRE(behavior.HasValue());
    REQUIRE(world->ProcessQueryBatch().HasValue());
    REQUIRE(behavior.Value()->FixedUpdate({}, {1.0 / 60.0}).HasValue());
    REQUIRE(world->ProcessQueryBatch().HasValue());
    context->Revoke();
    world.reset();
    REQUIRE(behavior.Value()->FixedUpdate({}, {1.0 / 60.0}).HasValue());
}
#endif
