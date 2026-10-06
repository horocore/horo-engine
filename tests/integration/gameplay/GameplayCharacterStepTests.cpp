#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

#if HORO_TEST_PHYSICS_NATIVE
namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace Horo::Character;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;

    struct CanonicalStepHost final {
        std::unique_ptr<PhysicsRuntime> runtime;
        std::unique_ptr<Runtime::RuntimeScene> scene;
        std::unique_ptr<PhysicsWorld> physics;
        std::unique_ptr<GameplayWorldComposition> gameplay;
        std::unique_ptr<CharacterWorld> character;
        CharacterControllerDescriptor descriptor;
        CharacterControllerHandle controller;
        PhysicsQueryFixture ramp;
        PhysicsQueryFixture obstacle;

        explicit CanonicalStepHost(const float height, const float stepLimit = 0.3F, const bool ceiling = false) {
            auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(created.HasValue());
            runtime = std::move(created).Value();
            const auto definition =
                Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, BehaviorTypeId::Parse("game.tests.dynamic_mover").Value());
            scene = Runtime::RuntimeScene::Create(definition, {7}).Value();
            physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            REQUIRE(
                physics->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
            const GameplayWorldSelection selection{.moduleId = "game.tests",
                                                   .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                   .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                   .physicsPermission = GameplayPhysicsPermission::Granted};
            gameplay = GameplayWorldComposition::Create(*scene, physics.get(), 1, selection).Value();
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            character = CharacterWorld::Prepare({1, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture(settings).Value()).Value();
            descriptor.sceneGeneration = 1;
            descriptor.characterWorld = character->Descriptor().identity;
            descriptor.physicsWorld = physics->Identity();
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.collisionRootPosition = {-0.75F, 0.77F, 0};
            descriptor.maximumStepHeightMeters = stepLimit;
            descriptor.collisionProfile = CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
            descriptor.queryChannel = PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
            descriptor.defaultMaterial = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 1,
                                          PhysicsMaterialSlotId::FromValue(1)};
            descriptor.selectors.requiredLayer = CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value();
            const PhysicsQueryFixtureDescriptor plane{.shape = PhysicsStaticPlaneShape{{0, 1, 0}, 0},
                                                      .layer = *descriptor.selectors.requiredLayer,
                                                      .profile = descriptor.collisionProfile,
                                                      .channel = descriptor.queryChannel};
            const auto added = physics->CreateQueryFixture(plane);
            REQUIRE(added.HasValue());
            ramp = added.Value();
            auto step = plane;
            step.shape = PhysicsBoxShape{{2, height * 0.5F, 2}};
            step.pose.translation = {2, height * 0.5F, 0};
            const auto createdStep = physics->CreateQueryFixture(step);
            REQUIRE(createdStep.HasValue());
            obstacle = createdStep.Value();
            if (ceiling) {
                auto roof = plane;
                roof.shape = PhysicsBoxShape{{4, 0.1F, 2}};
                roof.pose.translation = {0, 1.7F, 0};
                REQUIRE(physics->CreateQueryFixture(roof).HasValue());
            }

            controller = character->CreateController(descriptor).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter{*physics};
            REQUIRE(character->SpawnController(controller, adapter.Context(Expectations(0))).HasValue());
        }

        ~CanonicalStepHost() {
            if (gameplay)
                gameplay->Shutdown();
            if (character)
                character->Shutdown();
            if (physics)
                physics->Shutdown();
        }

        [[nodiscard]] CharacterPhysicsQueryExpectations Expectations(const std::uint64_t tick) const {
            const auto &owner = character->Descriptor();
            return {owner.sceneGeneration,  owner.identity, owner.physicsWorld,           owner.collisionFilterGeneration,
                    owner.originGeneration, tick,           owner.physicsSnapshotRevision};
        }

        [[nodiscard]] CharacterLocomotionSnapshot Move(const std::uint64_t tick, const Math::Vec3 velocity,
                                                       const std::int64_t nanoseconds = 16'666'667) {
            REQUIRE(character
                        ->QueueMovementCommand(
                            {.controller = controller, .tick = tick, .sequence = tick, .desiredVelocityMetersPerSecond = velocity})
                        .HasValue());
            CharacterPhysicsQueryAdapter adapter{*physics};
            REQUIRE(character
                        ->AdvanceFixedTick({.tick = tick,
                                            .sceneGeneration = 1,
                                            .fixedDelta = Duration::FromNanoseconds(nanoseconds),
                                            .query = adapter.Context(Expectations(tick))})
                        .HasValue());
            const auto snapshot = character->ControllerLocomotionSnapshot(controller).Value();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
            Runtime::SceneCommandBuffer commands;
            Math::Transform transform;
            transform.translation = snapshot.transform.position;
            const auto entity = *scene->View().Find({1});
            commands.SetLocalTransform(entity, transform);
            REQUIRE(scene->Commit(commands).HasValue());
            REQUIRE(scene->View().Get(entity).Value().localTransform->translation == snapshot.transform.position);
            return snapshot;
        }
    };

    TEST_CASE("Canonical Gameplay Scene capsule steps onto eligible native box landings", "[gameplay-physics][character][step][native]") {
        const float height = GENERATE(0.1F, 0.299F, 0.3F);
        CanonicalStepHost host{height};
        const auto client = host.gameplay->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
        PhysicsQueryDescriptor query{.world = host.physics->Identity(),
                                     .sceneGeneration = 1,
                                     .geometry = PhysicsCapsuleSweepQuery{host.descriptor.capsule, {1, 1.2F, 0}, {0, 1, 0}, {0, -1, 0}, 1},
                                     .filter = {.channel = host.descriptor.queryChannel,
                                                .requiredLayer = host.descriptor.selectors.requiredLayer,
                                                .blockingOnly = true},
                                     .collection = PhysicsQueryCollection::All,
                                     .maximumHitCount = 4};
        std::array<PhysicsQueryHit, 4> hits{};
        const auto observed = client.Submit({client.Identity(), host.physics->PublishedTick().publicationRevision, query}, hits);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().result.hitCount > 0);
        bool stepped{};
        CharacterLocomotionSnapshot snapshot;
        for (std::uint64_t tick = 1; tick <= 36; ++tick) {
            snapshot = host.Move(tick, {3, 0, 0});
            stepped = stepped || (static_cast<std::uint16_t>(snapshot.movement.collisions) &
                                  static_cast<std::uint16_t>(CharacterCollisionFlags::Step)) != 0;
        }
        REQUIRE(stepped);
        REQUIRE(snapshot.transform.position.x > 0.5F);
        REQUIRE(snapshot.transform.position.y == Catch::Approx(0.77F + height).margin(2.0e-3F));
        REQUIRE(snapshot.movement.grounded);
        REQUIRE(snapshot.movement.groundBody == host.obstacle.body);
        host.character->Shutdown();
        REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, host.descriptor).HasValue());
    }

    TEST_CASE("Canonical capsule curvature cannot climb over step limits or through ceilings",
              "[gameplay-physics][character][step][native][boundary]") {
        const bool ceiling = GENERATE(false, true);
        CanonicalStepHost host{ceiling ? 0.3F : 0.301F, 0.3F, ceiling};
        const auto initial = host.character->ControllerTransform(host.controller).Value().position;
        for (std::uint64_t tick = 1; tick <= 36; ++tick) {
            const auto snapshot = host.Move(tick, {3, 0, 0});
            REQUIRE(snapshot.transform.position.x < 0.0F);
            REQUIRE(snapshot.transform.position.y <= initial.y + 1.0e-4F);
        }
    }
}  // namespace
#endif
