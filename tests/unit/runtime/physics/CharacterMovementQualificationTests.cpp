#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        CharacterPhysicsQueryExpectations QueryExpectations(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
            return {world.sceneGeneration,  world.identity, world.physicsWorld,           world.collisionFilterGeneration,
                    world.originGeneration, tick,           world.physicsSnapshotRevision};
        }

        TEST_CASE("Movement qualification reports unavailable capsule queries in Null compositions",
                  "[physics][character][movement-qualification][headless]") {
            auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Null).Value();
            auto physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            auto character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, Settings(1)).Value();
            const auto controller = character->CreateController(ControllerDescriptor(character->Descriptor())).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter(*physics);
            RequireError(character->SpawnController(controller, adapter.Context(QueryExpectations(character->Descriptor(), 0))),
                         Physics::PhysicsErrors::CapabilityUnavailable);
            REQUIRE(character->PublishedTick().completedTick == 0);
            REQUIRE(character->ControllerTransform(controller).HasError());
        }

#if HORO_TEST_PHYSICS_NATIVE
        enum class ReferenceScene : std::uint8_t {
            Ramp,
            Stair,
            Ledge,
            Corner,
            Seam,
            Ceiling,
            ThinWall,
            Filter,
            HighSpeed
        };

        struct ReferenceOutcome final {
            Math::Vec3 position;
            bool grounded{};
        };

        /** @brief Owns actual canonical Physics geometry and its paired Character publication lifecycle. */
        struct ReferenceWorld final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime{
                Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value()};
            std::unique_ptr<Physics::PhysicsWorld> physics{runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value()};
            std::unique_ptr<CharacterWorld> character;
            CharacterControllerHandle controller;

            ReferenceWorld(const std::uint32_t queries = 64, const std::uint32_t iterations = 8) {
                REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
                CharacterWorldSettingsDescriptor settings;
                settings.capacities.maximumControllers = 1;
                settings.work.maximumQueriesPerTick = queries;
                settings.work.maximumMovementIterations = iterations;
                character =
                    CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, CharacterWorldSettings::Capture(settings).Value()).Value();
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
                REQUIRE(physics
                            ->AdvanceFixedTick(
                                {.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
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
        void ConfigureTerrain(ReferenceWorld &host, const ReferenceScene scene, ReferencePlan &plan) {
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
        void ConfigureBarriers(ReferenceWorld &host, const ReferenceScene scene, const bool reverse, ReferencePlan &plan) {
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
        ReferenceOutcome RunReference(const ReferenceScene scene, const bool reverse) {
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
            REQUIRE(
                ValidateCharacterLocomotionSnapshot(snapshot, host.character->ControllerDescriptor(host.controller).Value()).HasValue());
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
            return {snapshot.transform.position, snapshot.movement.grounded};
        }

        TEST_CASE("Canonical movement reference corpus meets terminal pose and bounded work contracts",
                  "[physics][character][movement-qualification][native]") {
            const auto scene =
                GENERATE(ReferenceScene::Ramp, ReferenceScene::Stair, ReferenceScene::Ledge, ReferenceScene::Corner, ReferenceScene::Seam,
                         ReferenceScene::Ceiling, ReferenceScene::ThinWall, ReferenceScene::Filter, ReferenceScene::HighSpeed);
            INFO("reference scene: " << static_cast<unsigned>(scene));
            const auto first = RunReference(scene, false);
            const auto replay = RunReference(scene, true);
            REQUIRE(first.position.x == Catch::Approx(replay.position.x).margin(1.0e-5F));
            REQUIRE(first.position.y == Catch::Approx(replay.position.y).margin(1.0e-5F));
            REQUIRE(first.position.z == Catch::Approx(replay.position.z).margin(1.0e-5F));
            REQUIRE(first.grounded == replay.grounded);
        }

        TEST_CASE("Canonical movement qualification publishes only swept travel at the iteration boundary",
                  "[physics][character][movement-qualification][native][capacity]") {
            ReferenceWorld host(64, 1);
            host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
            host.Add(Physics::PhysicsStaticPlaneShape{{-1, 0, 0}, -1});
            host.Spawn();
            CharacterMetricCapture capture;
            REQUIRE(host.Move({10, 0, 10}, capture).HasValue());
            const auto snapshot = host.character->ControllerLocomotionSnapshot(host.controller).Value();
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::IterationLimit);
            REQUIRE(snapshot.transform.position.x == Catch::Approx(0.75F - 0.02F / std::sqrt(2.0F)).margin(0.005F));
            REQUIRE(snapshot.transform.position.z == Catch::Approx(snapshot.transform.position.x).margin(0.005F));
            REQUIRE(capture.snapshot.movementIterations == 1);
            REQUIRE(host.character->PublishedTick().completedTick == 1);
        }

        TEST_CASE("Canonical movement qualification fences stale snapshots and retains detached results after shutdown",
                  "[physics][character][movement-qualification][native][lifecycle]") {
            ReferenceWorld host;
            host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
            host.Spawn();
            CharacterMetricCapture capture;
            REQUIRE(host.Move({1, 0, 0}, capture).HasValue());
            const auto before = host.character->ControllerLocomotionSnapshot(host.controller).Value();
            const auto descriptor = host.character->ControllerDescriptor(host.controller).Value();
            host.Add(Physics::PhysicsBoxShape{{0.5F, 0.5F, 0.5F}}, {10, 0, 0});
            RequireError(host.Move({1, 0, 0}, capture, 2), Physics::PhysicsErrors::QuerySnapshotStale);
            REQUIRE(capture.snapshot.failed);
            REQUIRE(host.character->PublishedTick().completedTick == 1);
            const auto after = host.character->ControllerLocomotionSnapshot(host.controller).Value();
            REQUIRE(after.stateRevision == before.stateRevision);
            REQUIRE(after.transform.position == before.transform.position);
            host.character->Shutdown();
            host.physics->Shutdown();
            REQUIRE(ValidateCharacterLocomotionSnapshot(before, descriptor).HasValue());
        }

        TEST_CASE("Canonical movement qualification preserves publication when query budget runs out",
                  "[physics][character][movement-qualification][native][capacity]") {
            ReferenceWorld host(1);
            host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
            host.Add(Physics::PhysicsStaticPlaneShape{{-1, 0, 0}, -1});
            host.Spawn();
            const auto before = host.character->ControllerTransform(host.controller).Value();
            CharacterMetricCapture capture;
            RequireError(host.Move({10, 0, 0}, capture), CharacterErrors::CapacityExceeded);
            REQUIRE(capture.snapshot.queries == 1);
            REQUIRE(capture.snapshot.failed);
            REQUIRE(capture.snapshot.publicationRevision == 0);
            REQUIRE(host.character->PublishedTick().completedTick == 0);
            REQUIRE(host.character->ControllerTransform(host.controller).Value().position == before.position);
        }
#endif
    }  // namespace
}  // namespace Horo::Character
