#include "CharacterReferenceSceneCorpusTestHelpers.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        TEST_CASE("Movement qualification reports unavailable capsule queries in Null compositions",
                  "[physics][character][movement-qualification][headless][character-reference]") {
            auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Null).Value();
            auto physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            auto character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, Settings(1)).Value();
            const auto controller = character->CreateController(ControllerDescriptor(character->Descriptor())).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter(*physics);
            RequireError(character->SpawnController(controller,
                                                    adapter.Context({character->Descriptor().sceneGeneration,
                                                                     character->Descriptor().identity, character->Descriptor().physicsWorld,
                                                                     character->Descriptor().collisionFilterGeneration,
                                                                     character->Descriptor().originGeneration, 0,
                                                                     character->Descriptor().physicsSnapshotRevision})),
                         Physics::PhysicsErrors::CapabilityUnavailable);
            REQUIRE(character->PublishedTick().completedTick == 0);
            REQUIRE(character->ControllerTransform(controller).HasError());
        }

#if HORO_TEST_PHYSICS_NATIVE
        using namespace ReferenceTest;

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
                  "[physics][character][movement-qualification][native][capacity][character-reference]") {
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
                  "[physics][character][movement-qualification][native][lifecycle][character-reference]") {
            ReferenceWorld host;
            host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
            host.Spawn();
            CharacterMetricCapture capture;
            REQUIRE(host.Move({1, 0, 0}, capture).HasValue());
            const auto before = host.character->ControllerLocomotionSnapshot(host.controller).Value();
            const auto descriptor = host.character->ControllerDescriptor(host.controller).Value();
            CharacterPhysicsQueryAdapter adapter(*host.physics);
            const auto stale = adapter.Context(QueryExpectations(host.character->Descriptor(), 2));
            REQUIRE(
                host.physics
                    ->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
            REQUIRE(host.character->RefreshPhysicsSnapshot(host.physics->Identity(), host.physics->PublishedTick().publicationRevision)
                        .HasValue());
            REQUIRE(stale.physicsSnapshotRevision != host.character->Descriptor().physicsSnapshotRevision);
            auto command = Movement(host.controller, 2, 2);
            command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(host.character->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(2);
            input.query = stale;
            input.metrics = &capture;
            RequireError(host.character->AdvanceFixedTick(input), CharacterErrors::QuerySnapshotStale);
            REQUIRE(capture.snapshot.queries == 0);
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
                  "[physics][character][movement-qualification][native][capacity][character-reference]") {
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
