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
TEST_CASE("Gameplay Scene and Character consume the same filtered canonical capsule evidence",
          "[gameplay-physics][host][character-filter]") {
    using namespace Horo::Character;
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto scene = Scene();
    auto physics = World(*runtime, 1);
    auto gameplay = GameplayWorldComposition::Create(*scene, physics.get(), 1, Native()).Value();
    auto client = gameplay->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
    CharacterWorldSettingsDescriptor settings;
    settings.capacities.maximumControllers = 1;
    auto character = CharacterWorld::Prepare({1, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture(settings).Value()).Value();
    CharacterControllerDescriptor descriptor;
    descriptor.sceneGeneration = 1;
    descriptor.characterWorld = character->Descriptor().identity;
    descriptor.physicsWorld = physics->Identity();
    descriptor.capsule = {0.25F, 0.5F};
    descriptor.collisionProfile = CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
    descriptor.queryChannel = PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
    descriptor.defaultMaterial = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 1,
                                  PhysicsMaterialSlotId::FromValue(1)};
    const auto layer = CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value();
    PhysicsQueryFixtureDescriptor fixture{.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                          .pose = {.translation = {1, 0, 0}},
                                          .layer = layer,
                                          .profile = descriptor.collisionProfile,
                                          .channel = descriptor.queryChannel,
                                          .trigger = true};
    REQUIRE(physics->CreateQueryFixture(fixture).HasValue());
    fixture.trigger = false;
    fixture.pose.translation = {2, 0, 0};
    const auto solid = physics->CreateQueryFixture(fixture).Value();
    auto controller = character->CreateController(descriptor).Value();
    REQUIRE(character->Activate().HasValue());
    CharacterPhysicsQueryAdapter adapter{*physics};
    const auto context = [&](const std::uint64_t tick) {
        const auto &owner = character->Descriptor();
        return adapter.Context({owner.sceneGeneration, owner.identity, owner.physicsWorld, owner.collisionFilterGeneration,
                                owner.originGeneration, tick, owner.physicsSnapshotRevision});
    };
    REQUIRE(character->SpawnController(controller, context(0)).HasValue());
    PhysicsQueryDescriptor query{.world = physics->Identity(),
                                 .sceneGeneration = 1,
                                 .geometry = PhysicsCapsuleSweepQuery{descriptor.capsule, {}, {0, 1, 0}, {1, 0, 0}, 2},
                                 .filter = {.channel = descriptor.queryChannel, .requiredLayer = layer, .blockingOnly = true},
                                 .collection = PhysicsQueryCollection::All,
                                 .maximumHitCount = 4};
    std::array<PhysicsQueryHit, 4> hits{};
    const auto observed = client.Submit({client.Identity(), physics->PublishedTick().publicationRevision, query}, hits);
    REQUIRE(observed.HasValue());
    REQUIRE(observed.Value().result.hitCount == 1);
    REQUIRE(hits[0].body == solid.body);
    CharacterMovementRequest movement{.controller = controller,
                                      .tick = 1,
                                      .sequence = 1,
                                      .desiredVelocityMetersPerSecond = Math::Vec3{8, 0, 0},
                                      .filterChange = CharacterCollisionSelectors{.requiredLayer = layer}};
    REQUIRE(character->QueueMovementCommand(movement).HasValue());
    REQUIRE(
        character
            ->AdvanceFixedTick({.tick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(250'000'000), .query = context(1)})
            .HasValue());
    const auto snapshot = character->ControllerLocomotionSnapshot(controller).Value();
    REQUIRE(snapshot.movement.contactCount == 1);
    REQUIRE(snapshot.movement.contacts[0].body == solid.body);
    REQUIRE(snapshot.transform.position.x > 1.0F);
    REQUIRE(snapshot.transform.position.x < 1.26F);
    Runtime::SceneCommandBuffer commands;
    Math::Transform transform;
    transform.translation = snapshot.transform.position;
    const auto entity = *scene->View().Find({1});
    commands.SetLocalTransform(entity, transform);
    REQUIRE(scene->Commit(commands).HasValue());
    REQUIRE(scene->View().Get(entity).Value().localTransform->translation == snapshot.transform.position);
    gameplay->Shutdown();
    character->Shutdown();
    physics->Shutdown();
    REQUIRE(character->ControllerTransform(controller).HasError());
    Physics::Test::RequireError(client.Submit({client.Identity(), physics->PublishedTick().publicationRevision, query}, hits),
                                PhysicsErrors::CapabilityRevoked);
}

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
#endif
