#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;

    GameplayWorldSelection Native(const GameplayPhysicsPermission permission = GameplayPhysicsPermission::Granted) {
        return {.moduleId = "game.tests",
                .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                .physicsPermission = permission};
    }

    GameplayWorldSelection Script(const GameplayPhysicsPermission permission = GameplayPhysicsPermission::Granted) {
        const std::filesystem::path root{HORO_TEST_SOURCE_ROOT};
        return {.moduleId = "game.tests",
                .scriptSource = root / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script",
                .scriptSidecar = root / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script.meta",
                .physicsPermission = permission};
    }

    std::unique_ptr<Runtime::RuntimeScene> Scene(const bool script = false, const std::uint64_t runtime = 7) {
        auto definition =
            Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1},
                                                 BehaviorTypeId::Parse(script ? "game.tests.physics_context" : "game.tests.dynamic_mover")
                                                     .Value());
        return Runtime::RuntimeScene::Create(definition, {runtime}).Value();
    }

    std::unique_ptr<PhysicsWorld> World(PhysicsRuntime &runtime, const std::uint64_t generation) {
        auto world = runtime.PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(runtime.IssueWorldIdentity().Value()).HasValue());
        REQUIRE(world
                    ->AdvanceFixedTick(
                        {.simulationTick = 1, .sceneGeneration = generation, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        return world;
    }

    Result<void> FailSolver(void *, const CancellationToken &) noexcept {
        return Result<void>::Failure(MakeError(PhysicsErrors::InitializationFailed));
    }

    PhysicsQueryCommand Command(const PhysicsQueryEventCapability &client, const PhysicsWorld &world, const std::uint64_t generation) {
        PhysicsQueryDescriptor descriptor;
        descriptor.world = client.Identity().world;
        descriptor.sceneGeneration = generation;
        descriptor.geometry = PhysicsRayQuery{.maximumDistanceMeters = 10};
        descriptor.filter.channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        return {client.Identity(), world.PublishedTick().publicationRevision, descriptor};
    }
}  // namespace

TEST_CASE("Unavailable editor and disabled host compositions inject stable PhysicsUnavailable into native and script execution",
          "[gameplay-physics][host]") {
    for (const bool script : {false, true}) {
        auto scene = Scene(script);
        auto gameplay = GameplayWorldComposition::Create(*scene, nullptr, 1, script ? Script() : Native());
        REQUIRE(gameplay.HasValue());
        const auto context = gameplay.Value()->PhysicsContext();
        Physics::Test::RequireError(context->Acquire("game.tests", 7, 1), GameplayErrors::PhysicsUnavailable);
        REQUIRE(gameplay.Value()->FixedUpdate({1.0 / 60.0}).HasValue());
        gameplay.Value()->Shutdown();
        Physics::Test::RequireError(context->Acquire("game.tests", 7, 1), PhysicsErrors::CapabilityRevoked);
    }
}

TEST_CASE("Explicit Null Physics never grants native or script access and never selects a solver fallback", "[gameplay-physics][host]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
    for (const bool script : {false, true}) {
        auto scene = Scene(script);
        auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
        auto gameplay = GameplayWorldComposition::Create(*scene, world.get(), 1, script ? Script() : Native());
        REQUIRE(gameplay.HasValue());
        Physics::Test::RequireError(gameplay.Value()->PhysicsContext()->Acquire("game.tests", 7, 1), GameplayErrors::PhysicsUnavailable);
        REQUIRE(world->State() == PhysicsWorldState::ActiveNull);
        REQUIRE(runtime->Mode() == PhysicsRuntimeMode::Null);
    }
}

TEST_CASE("Resolved disabled module policy rejects activation before native or script code", "[gameplay-physics][host]") {
    for (const bool script : {false, true}) {
        auto scene = Scene(script);
        auto selection = script ? Script() : Native();
        selection.enabled = false;
        Physics::Test::RequireError(GameplayWorldComposition::Create(*scene, nullptr, 1, selection), GameplayErrors::PhysicsUnavailable);
    }
}

#if HORO_TEST_PHYSICS_NATIVE
TEST_CASE("Production gameplay composition injects exact native and script Physics permissions", "[gameplay-physics][host]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    for (const bool script : {false, true}) {
        for (const auto permission : {GameplayPhysicsPermission::Denied, GameplayPhysicsPermission::Granted}) {
            auto scene = Scene(script);
            auto world = World(*runtime, 1);
            auto gameplay = GameplayWorldComposition::Create(*scene, world.get(), 1, script ? Script(permission) : Native(permission));
            REQUIRE(gameplay.HasValue());
            const auto acquired = gameplay.Value()->PhysicsContext()->Acquire("game.tests", 7, 1);
            if (permission == GameplayPhysicsPermission::Denied)
                Physics::Test::RequireError(acquired, GameplayErrors::PhysicsPermissionDenied);
            else {
                REQUIRE(acquired.HasValue());
                REQUIRE(acquired.Value().Identity().world == world->Identity());
            }
            REQUIRE(gameplay.Value()->FixedUpdate({1.0 / 60.0}).HasValue());
        }
    }
}

TEST_CASE("Module unload scene unload and play stop revoke retained clients before world storage", "[gameplay-physics][host]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    for (const int lifecycle : {0, 1, 2}) {
        auto scene = Scene();
        auto world = World(*runtime, 1);
        auto gameplay = GameplayWorldComposition::Create(*scene, world.get(), 1, Native()).Value();
        auto client = gameplay->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
        const auto command = Command(client, *world, 1);
        auto pending = client.SubmitBatch(std::span{&command, 1}).Value();
        if (lifecycle == 0)
            gameplay->UnloadModule();
        else
            gameplay->Shutdown();
        Physics::Test::RequireError(pending.Poll(), PhysicsErrors::CapabilityRevoked);
        std::array<PhysicsQueryHit, 1> hits{};
        Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
        REQUIRE(world->State() == PhysicsWorldState::ActiveSolver);
        if (lifecycle == 1)
            REQUIRE(world->UnloadScene().HasValue());
        else if (lifecycle == 2)
            world->Shutdown();
        world.reset();
        Physics::Test::RequireError(pending.Poll(), PhysicsErrors::CapabilityRevoked);
        Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
    }
}

TEST_CASE("Same Scene ID in simultaneous and replacement play worlds never routes a retained client", "[gameplay-physics][host]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto firstScene = Scene();
    auto secondScene = Scene();
    auto firstWorld = World(*runtime, 1);
    auto secondWorld = World(*runtime, 2);
    auto first = GameplayWorldComposition::Create(*firstScene, firstWorld.get(), 1, Native()).Value();
    auto second = GameplayWorldComposition::Create(*secondScene, secondWorld.get(), 2, Native()).Value();
    auto stale = first->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
    auto live = second->PhysicsContext()->Acquire("game.tests", 7, 2).Value();
    const auto staleCommand = Command(stale, *firstWorld, 1);
    const auto liveCommand = Command(live, *secondWorld, 2);
    std::array<PhysicsQueryHit, 1> hits{};
    Physics::Test::RequireError(stale.Submit(liveCommand, hits), PhysicsErrors::HandleWorldMismatch);
    Physics::Test::RequireError(second->PhysicsContext()->Acquire("game.tests", 7, 1), PhysicsErrors::HandleWorldMismatch);
    first->Shutdown();
    firstWorld.reset();
    auto replacementWorld = World(*runtime, 3);
    auto replacement = GameplayWorldComposition::Create(*firstScene, replacementWorld.get(), 3, Native()).Value();
    REQUIRE(replacement->PhysicsContext()->Acquire("game.tests", 7, 3).HasValue());
    Physics::Test::RequireError(stale.Submit(staleCommand, hits), PhysicsErrors::CapabilityRevoked);
    REQUIRE(live.Submit(liveCommand, hits).HasValue());
}

TEST_CASE("Real headless factories pair equal Scene IDs by candidate generation not mutable Scene lookup", "[gameplay-physics][host]") {
    using namespace Horo::Network;
    auto definition = Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, BehaviorTypeId::Parse("game.tests.physics_context").Value());
    auto factories =
        ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition)), {}, Script());
    const auto make = [&](const NetworkModeServiceKind kind) {
        return factories
            .services[static_cast<std::size_t>(kind)]({kind, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}})
            .Value();
    };
    auto firstScene = make(NetworkModeServiceKind::Scene);
    auto firstPhysics = make(NetworkModeServiceKind::Physics);
    auto secondScene = make(NetworkModeServiceKind::Scene);
    auto secondPhysics = make(NetworkModeServiceKind::Physics);
    // Prepare all Scenes first: the old Scene-ID map would route both worlds to the second Scene.
    REQUIRE(firstScene->Prepare().HasValue());
    REQUIRE(secondScene->Prepare().HasValue());
    REQUIRE(firstPhysics->Prepare().HasValue());
    REQUIRE(secondPhysics->Prepare().HasValue());
    REQUIRE(firstScene->Activate().HasValue());
    REQUIRE(secondScene->Activate().HasValue());
    REQUIRE(firstPhysics->Activate().HasValue());
    REQUIRE(secondPhysics->Activate().HasValue());
    const auto first = InspectHeadlessGameplayContext(*firstPhysics);
    const auto second = InspectHeadlessGameplayContext(*secondPhysics);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->Binding().scene == second->Binding().scene);
    REQUIRE(first->Binding().sceneGeneration != second->Binding().sceneGeneration);
    REQUIRE(first->Binding().world != second->Binding().world);
    REQUIRE(first->Acquire("game.tests", 7, first->Binding().sceneGeneration).HasValue());
    CancellationSource cancellation;
    const auto token = cancellation.Token();
    const Runtime::FixedStepContext tick{1, Duration::FromNanoseconds(16'666'667), token};
    REQUIRE(firstPhysics->RunFixedTick(tick).HasValue());
    REQUIRE(secondPhysics->RunFixedTick(tick).HasValue());
    // Even a direct Scene-service retirement closes admission while the paired Physics
    // service still pins Scene storage for its ordered behavior/module teardown.
    firstScene->Shutdown();
    Physics::Test::RequireError(first->Acquire("game.tests", 7, first->Binding().sceneGeneration), PhysicsErrors::CapabilityRevoked);
    firstPhysics->Shutdown();
    REQUIRE(second->Acquire("game.tests", 7, second->Binding().sceneGeneration).HasValue());
    secondPhysics->Shutdown();
    secondScene->Shutdown();
}

TEST_CASE("World failure invalidates retained clients and batches while world storage is still alive", "[gameplay-physics][host]") {
    JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical, &jobs).Value();
    auto scene = Scene();
    auto world = World(*runtime, 1);
    auto gameplay = GameplayWorldComposition::Create(*scene, world.get(), 1, Native()).Value();
    auto client = gameplay->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
    const auto command = Command(client, *world, 1);
    auto pending = client.SubmitBatch(std::span{&command, 1}).Value();
    const PhysicsSolverJob job{.execute = FailSolver};
    const PhysicsSolverJobBatch batch{.jobs = &job, .jobCount = 1, .joinTimeout = Duration::FromMilliseconds(500)};
    REQUIRE(world
                ->AdvanceFixedTick(
                    {.simulationTick = 2, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667), .solverJobs = batch})
                .HasError());
    REQUIRE(world->State() == PhysicsWorldState::Failed);
    std::array<PhysicsQueryHit, 1> hits{};
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityStale);
    Physics::Test::RequireError(pending.Poll(), PhysicsErrors::CapabilityStale);
    gameplay->Shutdown();
    world.reset();
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
    // World failure already committed the batch's first terminal stale outcome.
    Physics::Test::RequireError(pending.Poll(), PhysicsErrors::CapabilityStale);
    runtime.reset();
    jobs.Shutdown(ShutdownPolicy::Cancel);
}

TEST_CASE("Gameplay-authorized canonical clearance preserves Character feet beneath a moving ceiling", "[gameplay-physics][host][stance]") {
    using namespace Horo::Character;
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto scene = Scene();
    auto physics = World(*runtime, 1);
    auto gameplay = GameplayWorldComposition::Create(*scene, physics.get(), 1, Native()).Value();
    const auto channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
    const auto profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value();
    const auto layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value();
    const auto fixture = [&](const PhysicsQueryFixtureShape &shape, const Math::Vec3 position,
                             const PhysicsQueryFixtureResponse response = PhysicsQueryFixtureResponse::Block) {
        return physics
            ->CreateQueryFixture({.shape = shape,
                                  .pose = {.translation = position, .rotation = Math::Quaternion::Identity()},
                                  .layer = layer,
                                  .profile = profile,
                                  .channel = channel,
                                  .response = response})
            .Value();
    };
    const auto floor = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, -0.27F, 0});
    auto ceiling = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, 2.5F, 0});
    auto character = CharacterWorld::Prepare({1, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture({}).Value()).Value();
    const auto &world = character->Descriptor();
    CharacterControllerDescriptor descriptor;
    descriptor.sceneGeneration = 1;
    descriptor.characterWorld = world.identity;
    descriptor.physicsWorld = physics->Identity();
    descriptor.collisionRootPosition = {0, 1, 0};
    descriptor.collisionProfile = profile;
    descriptor.queryChannel = channel;
    descriptor.crouchedCapsule = PhysicsCapsuleShape{0.5F, 0.25F};
    descriptor.defaultMaterial = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 1,
                                  PhysicsMaterialSlotId::FromValue(1)};
    const auto handle = character->CreateController(descriptor).Value();
    REQUIRE(character->Activate().HasValue());
    const auto clearance = [&](const std::uint64_t tick) {
        return gameplay->PhysicsContext()->AcquireCharacterClearance("game.tests", 7,
                                                                     {world.sceneGeneration, world.identity, world.physicsWorld,
                                                                      world.collisionFilterGeneration, world.originGeneration, tick,
                                                                      physics->PublishedTick().publicationRevision});
    };
    auto spawnQuery = clearance(0).Value();
    REQUIRE(character->SpawnController(handle, spawnQuery.Context()).HasValue());
    const auto advance = [&](const std::uint64_t tick, const CharacterStanceIntent stance) {
        REQUIRE(character->QueueMovementCommand({.controller = handle, .tick = tick, .sequence = tick, .stance = stance}).HasValue());
        auto query = clearance(tick).Value();
        return character->AdvanceFixedTick(
            {.tick = tick, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667), .query = query.Context()});
    };
    REQUIRE(advance(1, CharacterStanceIntent::Crouch).HasValue());
    REQUIRE(character->ControllerTransform(handle).Value().position.y == 0.75F);
    REQUIRE(physics->DestroyQueryFixture(ceiling).HasValue());
    ceiling = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, 1.85F, 0});
    for (std::uint64_t tick = 2; tick <= 8; ++tick) {
        REQUIRE(advance(tick, CharacterStanceIntent::Stand).HasValue());
        const auto snapshot = character->ControllerLocomotionSnapshot(handle).Value();
        REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Blocked);
        REQUIRE(snapshot.stance == CharacterStance::Crouched);
        REQUIRE(snapshot.transform.position.y == 0.75F);
    }
    REQUIRE(physics->DestroyQueryFixture(ceiling).HasValue());
    ceiling = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, 2.5F, 0});
    REQUIRE(advance(9, CharacterStanceIntent::Stand).HasValue());
    REQUIRE(character->ControllerTransform(handle).Value().position.y == 1.0F);
    REQUIRE(character->ControllerLocomotionSnapshot(handle).Value().stance == CharacterStance::Standing);
    REQUIRE(physics->DestroyQueryFixture(ceiling).HasValue());
    ceiling = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, 1.5F, 0});
    REQUIRE(advance(10, CharacterStanceIntent::Crouch).HasValue());
    REQUIRE(character->ControllerLocomotionSnapshot(handle).Value().movement.shapeChange->status == CharacterShapeChangeStatus::Blocked);
    REQUIRE(character->ControllerTransform(handle).Value().position.y == 1.0F);
    REQUIRE(physics->DestroyQueryFixture(ceiling).HasValue());
    ceiling = fixture(PhysicsBoxShape{{5, 0.25F, 5}}, {0, 2.5F, 0});
    // A retained production adapter must fail before resized publication on retirement.
    auto retained = clearance(11).Value();
    SECTION("module retirement") {
        gameplay->Shutdown();
    }
    SECTION("Physics world shutdown") {
        physics->Shutdown();
    }
    SECTION("stale Physics publication") {
        REQUIRE(physics->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
    }
    REQUIRE(character->QueueMovementCommand({.controller = handle, .tick = 11, .sequence = 11, .stance = CharacterStanceIntent::Crouch})
                .HasValue());
    REQUIRE(character
                ->AdvanceFixedTick(
                    {.tick = 11, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667), .query = retained.Context()})
                .HasError());
    REQUIRE(character->PublishedTick().completedTick == 10);
    REQUIRE(character->ControllerTransform(handle).Value().position.y == 1.0F);
    character->Shutdown();
    gameplay->Shutdown();
    if (physics->State() == PhysicsWorldState::ActiveSolver) {
        REQUIRE(physics->DestroyQueryFixture(ceiling).HasValue());
        REQUIRE(physics->DestroyQueryFixture(floor).HasValue());
    }
}
#endif
