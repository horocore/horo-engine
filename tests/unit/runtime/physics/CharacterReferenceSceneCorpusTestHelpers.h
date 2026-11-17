#pragma once
/** @file CharacterReferenceSceneCorpusTestHelpers.h @brief Reusable native reference recipes and independent authored movement oracles. */
#include "CharacterReferenceSceneCorpus.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>
#include <cmath>

#if HORO_TEST_PHYSICS_NATIVE
namespace Horo::Character::ReferenceTest {
    using namespace TestDetail;
    using Corpus::ReferenceScene;

    inline CharacterPhysicsQueryExpectations QueryExpectations(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
        return {world.sceneGeneration,  world.identity, world.physicsWorld,           world.collisionFilterGeneration,
                world.originGeneration, tick,           world.physicsSnapshotRevision};
    }

    struct ReferenceOutcome final {
        Math::Vec3 position;
        bool grounded{};
        Math::Vec3 velocity{};
    };

    /** @brief Owns actual canonical Physics geometry and its paired Character publication lifecycle. */
    struct ReferenceWorld final {
        std::unique_ptr<Physics::PhysicsRuntime> runtime{Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value()};
        std::unique_ptr<Physics::PhysicsWorld> physics{runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value()};
        std::unique_ptr<CharacterWorld> character;
        CharacterControllerHandle controller;

        ReferenceWorld(const std::uint32_t queries = 64, const std::uint32_t iterations = 8) {
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.work.maximumQueriesPerTick = queries;
            settings.work.maximumMovementIterations = iterations;
            character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture(settings).Value()).Value();
        }

        void Add(const Physics::PhysicsQueryFixtureShape &shape, const Math::Vec3 position = {}, const bool trigger = false) {
            const auto policy = ControllerDescriptor(character->Descriptor());
            REQUIRE(physics
                        ->CreateQueryFixture({.shape = shape,
                                              .pose = {.translation = position},
                                              .layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                                              .profile = policy.collisionProfile,
                                              .channel = policy.queryChannel,
                                              .trigger = trigger})
                        .HasValue());
        }

        void Spawn(const Math::Vec3 position = {0, 0.77F, 0}, const Math::Vec3 gravity = {}) {
            REQUIRE(
                physics->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
            REQUIRE(character->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasValue());
            auto descriptor = ControllerDescriptor(character->Descriptor());
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.collisionRootPosition = position;
            descriptor.gravity = gravity;
            controller = character->CreateController(descriptor).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter(*physics);
            REQUIRE(character->SpawnController(controller, adapter.Context(QueryExpectations(character->Descriptor(), 0))).HasValue());
        }

        Result<void> Move(const Math::Vec3 velocity, CharacterMetricCapture &capture, const std::uint64_t tick = 1) {
            auto command = Movement(controller, tick, tick);
            command.desiredVelocityMetersPerSecond = velocity;
            REQUIRE(character->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(tick);
            input.fixedDelta = Duration::FromNanoseconds(100'000'000);
            CharacterPhysicsQueryAdapter adapter(*physics);
            input.query = adapter.Context(QueryExpectations(character->Descriptor(), tick));
            input.metrics = &capture;
            return character->AdvanceFixedTick(input);
        }
    };

    struct ReferencePlan final {
        Math::Vec3 velocity{10, 0, 0};
        Math::Vec3 start{0, 0.77F, 0};
        Math::Vec3 gravity;
        ReferenceOutcome expected{{1, 0.77F, 0}, true};
    };

    /** @brief Authors support geometry and its independent elevation expectation. */
    inline void ConfigureTerrain(ReferenceWorld &host, const ReferenceScene scene, ReferencePlan &plan) {
        switch (scene) {
            case ReferenceScene::Ramp: {
                const Math::Vec3 normal{-0.5F, std::sqrt(0.75F), 0};
                host.Add(Physics::PhysicsStaticPlaneShape{normal, 0});
                plan.start.y = (0.25F + 0.5F * normal.y) / normal.y + 0.02F;
                plan.velocity.x = 6;
                plan.expected.position = {0.6F, plan.start.y + 0.6F / std::sqrt(3.0F), 0};
                break;
            }
            case ReferenceScene::Stair:
                host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
                host.Add(Physics::PhysicsBoxShape{{1.5F, 0.075F, 2}}, {2, 0.075F, 0});
                plan.expected.position.y += 0.15F;
                break;
            case ReferenceScene::Ledge:
                host.Add(Physics::PhysicsBoxShape{{1, 0.5F, 2}}, {-0.75F, -0.5F, 0});
                plan.gravity = {0, -9.81F, 0};
                plan.expected.position.y -= 0.04905F;
                plan.expected.grounded = false;
                break;
            default:
                break;
        }
    }

    /** @brief Authors barriers on level support with capsule-radius and skin travel bounds. */
    inline void ConfigureBarriers(ReferenceWorld &host, const ReferenceScene scene, const bool reverse, ReferencePlan &plan) {
        const Physics::PhysicsStaticPlaneShape wall{{-1, 0, 0}, -1};
        host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
        switch (scene) {
            case ReferenceScene::Corner:
                host.Add(reverse ? Physics::PhysicsStaticPlaneShape{{0, 0, -1}, -1} : wall);
                host.Add(reverse ? wall : Physics::PhysicsStaticPlaneShape{{0, 0, -1}, -1});
                plan.velocity.z = 10;
                plan.expected.position.x = plan.expected.position.z = 0.75F - 0.02F / std::sqrt(2.0F);
                break;
            case ReferenceScene::Seam:
                host.Add(wall);
                host.Add(wall);
                plan.velocity.z = 10;
                plan.expected.position.x = 0.75F - 0.02F / std::sqrt(2.0F);
                plan.expected.position.z = 1;
                break;
            case ReferenceScene::Ceiling:
                host.Add(Physics::PhysicsStaticPlaneShape{{0, -1, 0}, -2});
                plan.velocity = {0, 10, 0};
                plan.expected = {{0, 1.23F, 0}, false};
                break;
            case ReferenceScene::ThinWall:
                host.Add(Physics::PhysicsBoxShape{{0.01F, 2, 2}}, {1, 0, 0});
                plan.velocity.x = 100;
                plan.expected.position.x = 0.72F;
                break;
            case ReferenceScene::Filter:
                host.Add(Physics::PhysicsBoxShape{{0.01F, 2, 2}}, {0.5F, 0, 0}, true);
                break;
            case ReferenceScene::HighSpeed:
                host.Add(wall);
                plan.velocity.x = 1280;
                plan.expected.position.x = 0.73F;
                break;
            default:
                break;
        }
    }

    /** @brief Executes a fresh world against authored terminal poses without deriving golds from observed results. */
    inline ReferenceOutcome RunReference(const ReferenceScene scene, const bool reverse) {
        ReferenceWorld host;
        ReferencePlan plan;
        if (scene == ReferenceScene::Ramp || scene == ReferenceScene::Stair || scene == ReferenceScene::Ledge)
            ConfigureTerrain(host, scene, plan);
        else
            ConfigureBarriers(host, scene, reverse, plan);
        host.Spawn(plan.start, plan.gravity);
        CharacterMetricCapture capture;
        const auto advanced = host.Move(plan.velocity, capture);
        if (advanced.HasError())
            UNSCOPED_INFO(advanced.ErrorValue().message);
        REQUIRE(advanced.HasValue());
        const auto snapshot = host.character->ControllerLocomotionSnapshot(host.controller).Value();
        REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, host.character->ControllerDescriptor(host.controller).Value()).HasValue());
        INFO("native terminal pose: " << snapshot.transform.position.x << ", " << snapshot.transform.position.y << ", "
                                      << snapshot.transform.position.z << "; grounded=" << snapshot.movement.grounded
                                      << "; termination=" << static_cast<unsigned>(snapshot.movement.termination));
        // Native contact tolerance is 5 mm; headless exact oracles retain their tighter authored tolerances.
        REQUIRE(snapshot.transform.position.x == Catch::Approx(plan.expected.position.x).margin(0.005F));
        REQUIRE(snapshot.transform.position.y == Catch::Approx(plan.expected.position.y).margin(0.005F));
        REQUIRE(snapshot.transform.position.z == Catch::Approx(plan.expected.position.z).margin(0.005F));
        REQUIRE(snapshot.movement.grounded == plan.expected.grounded);
        REQUIRE(snapshot.movement.termination == CharacterMovementTermination::Complete);
        REQUIRE(capture.snapshot.queries > 0);
        RequireMovementBudget(*host.character, capture);
        REQUIRE_FALSE(capture.snapshot.failed);
        REQUIRE(capture.snapshot.publicationRevision == host.character->PublishedTick().publicationRevision);
        return {snapshot.transform.position, snapshot.movement.grounded, snapshot.movement.achievedVelocityMetersPerSecond};
    }

}  // namespace Horo::Character::ReferenceTest
#endif
