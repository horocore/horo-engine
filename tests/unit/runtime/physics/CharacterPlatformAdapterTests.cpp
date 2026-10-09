#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#if HORO_TEST_PHYSICS_NATIVE
namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        /** @brief Owns a canonical resident body and performs actual Physics steps for adapter boundary coverage. */
        struct NativePlatformScenario final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::unique_ptr<Physics::PhysicsWorld> world;
            Physics::PhysicsBodyDescriptor descriptor;
            Physics::BodyHandle body;

            NativePlatformScenario(const Physics::PhysicsWorldId identity, const Physics::PhysicsMotionType motion,
                                   const Math::Vec3 position = {}) {
                runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value();
                world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
                REQUIRE(world->Activate(identity).HasValue());
                descriptor.shape = world->CreateSceneShape(Physics::PhysicsBoxShape{}).Value();
                descriptor.motion = motion;
                descriptor.pose.translation = position;
                if (motion == Physics::PhysicsMotionType::Dynamic)
                    descriptor.mass = Physics::PhysicsMass{1};
                body = world->CreateSceneBody({descriptor}).Value();
            }

            void Step(const std::uint64_t tick) {
                REQUIRE(world
                            ->AdvanceFixedTick(
                                {.simulationTick = tick, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
            }
        };

        TEST_CASE("Canonical platform pose reads retain owned shape identity and fence publication changes",
                  "[physics][native][character][platform]") {
            NativePlatformScenario scenario(PhysicsWorldId(955), Physics::PhysicsMotionType::Kinematic, {3, 0, 4});
            auto &world = scenario.world;
            const auto &descriptor = scenario.descriptor;
            const auto body = scenario.body;
            CharacterPhysicsQueryAdapter adapter(*world);
            const auto early = adapter.Context({61, WorldId(), world->Identity(), 91, 101, 1, 0});
            RequireError(early.platformBody(early.context, body, descriptor.shape, 0), Physics::PhysicsErrors::QuerySnapshotStale);
            scenario.Step(1);
            const CharacterPhysicsQueryExpectations
                expected{61, WorldId(), world->Identity(), 91, 101, 1, world->PublishedTick().publicationRevision};
            const auto query = adapter.Context(expected);
            const auto copied = query.platformBody(query.context, body, descriptor.shape, expected.physicsSnapshotRevision).Value().value();
            REQUIRE(copied.body == body);
            REQUIRE(copied.shape == descriptor.shape);
            REQUIRE(copied.pose == descriptor.pose);
            REQUIRE(copied.motion == Physics::PhysicsMotionType::Kinematic);
            auto staleShape = descriptor.shape;
            ++staleShape.slot.generation;
            REQUIRE_FALSE(query.platformBody(query.context, body, staleShape, expected.physicsSnapshotRevision).Value());
            RequireError(query.platformBody(query.context, body, descriptor.shape, expected.physicsSnapshotRevision + 1),
                         Physics::PhysicsErrors::QuerySnapshotStale);
            auto staleBody = body;
            ++staleBody.slot.generation;
            REQUIRE_FALSE(query.platformBody(query.context, staleBody, descriptor.shape, expected.physicsSnapshotRevision).Value());
            const auto other = world->CreateSceneBody({descriptor}).Value();
            REQUIRE(other != body);
            REQUIRE(world->PublishedTick().publicationRevision != expected.physicsSnapshotRevision);
            RequireError(query.platformBody(query.context, body, descriptor.shape, expected.physicsSnapshotRevision),
                         Physics::PhysicsErrors::QuerySnapshotStale);
            auto retirement = world->PrepareSceneGroup({}, {}).Value();
            const std::array retired{body};
            REQUIRE(retirement->PrepareRetirement(retired, {}, {}).HasValue());
            REQUIRE(retirement->ValidatePublication().HasValue());
            retirement->Publish();
            const auto revision = world->PublishedTick().publicationRevision;
            REQUIRE_FALSE(query.platformBody(query.context, body, descriptor.shape, revision).Value());
            const auto replacement = world->CreateSceneBody({descriptor}).Value();
            REQUIRE(replacement != body);
            REQUIRE_FALSE(query.platformBody(query.context, body, descriptor.shape, world->PublishedTick().publicationRevision).Value());
            world->Shutdown();
            REQUIRE(copied.pose == descriptor.pose);
            REQUIRE(query.platformBody(query.context, body, descriptor.shape, expected.physicsSnapshotRevision).HasError());
        }

        TEST_CASE("Canonical query-only ground remains usable without a resident attachment frame",
                  "[physics][native][character][platform]") {
            NativePlatformScenario scenario(PhysicsWorldId(9552), Physics::PhysicsMotionType::Static, {100, 0, 0});
            auto &physics = *scenario.world;
            // Pair the Character candidate with this actual canonical world, not a synthetic body provider.
            auto owner = WorldDescriptor();
            owner.physicsWorld = physics.Identity();
            auto character = CharacterWorld::Prepare(owner, Settings(1)).Value();
            auto descriptor = ControllerDescriptor(character->Descriptor());
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.collisionRootPosition = {0, 0.77F, 0};
            const auto fixture =
                physics
                    .CreateQueryFixture({.shape = Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0},
                                         .layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                                         .profile = descriptor.collisionProfile,
                                         .channel = descriptor.queryChannel})
                    .Value();
            CharacterPhysicsQueryAdapter adapter(physics);
            const auto early = adapter.Context({61, character->Descriptor().identity, physics.Identity(), 91, 101, 1, 1});
            REQUIRE(physics.PublishedTick().completedTick == 0);
            REQUIRE_FALSE(early.platformBody(early.context, fixture.body, fixture.shape, 1).Value());
            REQUIRE_FALSE(early.platformBody(early.context, scenario.body, scenario.descriptor.shape, 1).Value());
            scenario.Step(1);
            const auto bodyEvidence =
                early.platformBody(early.context, scenario.body, scenario.descriptor.shape, physics.PublishedTick().publicationRevision)
                    .Value();
            REQUIRE(bodyEvidence.has_value());
            REQUIRE(bodyEvidence->pose == scenario.descriptor.pose);
            REQUIRE(character->RefreshPhysicsSnapshot(physics.Identity(), physics.PublishedTick().publicationRevision).HasValue());
            const auto handle = character->CreateController(descriptor).Value();
            REQUIRE(character->Activate().HasValue());
            auto expected = CharacterPhysicsQueryExpectations{61, character->Descriptor().identity,           physics.Identity(), 91, 101,
                                                              0,  physics.PublishedTick().publicationRevision};
            REQUIRE(character->SpawnController(handle, adapter.Context(expected)).HasValue());
            REQUIRE(character->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());
            expected.tick = 1;
            auto input = FixedTick(1);
            input.query = adapter.Context(expected);
            REQUIRE(character->AdvanceFixedTick(input).HasValue());
            const auto snapshot = character->ControllerLocomotionSnapshot(handle).Value();
            REQUIRE(snapshot.movement.grounded);
            REQUIRE(snapshot.movement.groundBody == fixture.body);
            REQUIRE_FALSE(snapshot.movement.platformAttachment);
            REQUIRE_FALSE(snapshot.movement.platformAttached);
            REQUIRE(snapshot.movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Unavailable);
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
        }

        TEST_CASE("Canonical attachment evidence rejects reads after actual fixed step pose and shape mutation",
                  "[physics][native][character][platform]") {
            NativePlatformScenario scenario(PhysicsWorldId(9551), Physics::PhysicsMotionType::Dynamic);
            auto &world = scenario.world;
            const auto &descriptor = scenario.descriptor;
            const auto body = scenario.body;
            scenario.Step(1);
            const auto revision = world->PublishedTick().publicationRevision;
            CharacterPhysicsQueryAdapter adapter(*world);
            const auto query = adapter.Context({61, WorldId(), world->Identity(), 91, 101, 2, revision});
            const auto first = query.platformBody(query.context, body, descriptor.shape, revision).Value().value();
            const auto replacementShape = world->CreateSceneShape(Physics::PhysicsSphereShape{}).Value();
            REQUIRE(world->PublishedTick().publicationRevision == revision);
            const Physics::PhysicsStructuralCommand mutation{.order = {.simulationTick = 2,
                                                                       .worldGeneration = world->Identity().Value(),
                                                                       .sceneGeneration = 61,
                                                                       .targetKind = Physics::PhysicsCommandTargetKind::Body,
                                                                       .targetIdentity = static_cast<std::uint64_t>(body.slot.index) + 1,
                                                                       .commandKind = Physics::PhysicsStructuralCommandKind::Change,
                                                                       .source = Physics::PhysicsCommandSourceId::Create(1).Value(),
                                                                       .sourceSequence = 1},
                                                             .bodyMutation =
                                                                 Physics::PhysicsBodyMutation{.body = body, .shape = replacementShape}};
            REQUIRE(world->QueueStructuralCommand(mutation).HasValue());
            REQUIRE(world->PublishedTick().publicationRevision == revision);
            REQUIRE(query.platformBody(query.context, body, descriptor.shape, revision).Value()->pose == first.pose);
            scenario.Step(2);
            RequireError(query.platformBody(query.context, body, descriptor.shape, revision), Physics::PhysicsErrors::QuerySnapshotStale);
            const auto currentRevision = world->PublishedTick().publicationRevision;
            REQUIRE_FALSE(query.platformBody(query.context, body, descriptor.shape, currentRevision).Value());
            const auto second = query.platformBody(query.context, body, replacementShape, currentRevision).Value().value();
            REQUIRE(second.shape == replacementShape);
            REQUIRE(second.pose.translation.y < first.pose.translation.y);
            REQUIRE(first.shape == descriptor.shape);
        }
    }  // namespace
}  // namespace Horo::Character
#endif
