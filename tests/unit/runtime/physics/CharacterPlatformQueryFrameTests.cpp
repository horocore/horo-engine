#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        TEST_CASE("Captured-only Character providers reject a predicted support frame", "[physics][character][platform][frame]") {
            auto expected = CharacterPhysicsQueryExpectations{61, WorldId(), PhysicsWorldId(), 91, 101, 2, 1};
            OverlapProbe probe;
            auto context = probe.Context(CharacterWorldDescriptor{61, WorldId(), PhysicsWorldId(), 91, 101, 1}, 2);
            context.bodyPoseFrame =
                Physics::PhysicsQueryBodyPoseFrame{{expected.physicsWorld, {1, 1}}, {expected.physicsWorld, {2, 1}}, {}, {}, 1, 2};
            RequireError(ValidateCharacterPhysicsQueryContext(context, expected), CharacterErrors::OperationUnsupported);
        }

        TEST_CASE("Null Physics rejects a framed capsule query with its explicit capability failure",
                  "[physics][character][platform][frame]") {
            auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Null).Value();
            auto physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(PhysicsWorldId()).HasValue());
            Physics::PhysicsQueryDescriptor query;
            query.world = physics->Identity();
            query.sceneGeneration = 61;
            query.bodyPoseFrame = Physics::PhysicsQueryBodyPoseFrame{{query.world, {1, 1}}, {query.world, {2, 1}}, {}, {}, 1, 1};
            std::array<Physics::PhysicsQueryHit, 1> hits;
            RequireError(physics->Query(query, hits), Physics::PhysicsErrors::CapabilityUnavailable);
        }

#if HORO_TEST_PHYSICS_NATIVE
        /** @brief Owns a real resident Jolt platform, an explicit schema and an independent Character world. */
        struct ResidentPlatform final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::unique_ptr<Physics::PhysicsWorld> physics;
            std::unique_ptr<CharacterWorld> character;
            std::unique_ptr<CharacterPhysicsQueryAdapter> adapter;
            CharacterControllerHandle controller;
            Physics::BodyHandle body;
            Physics::ShapeHandle shape;
            std::optional<Physics::PhysicsQueryFixture> wall;
            Physics::CollisionLayerId layer;
            Physics::CollisionProfileId profile;
            Physics::PhysicsQueryChannelId channel;

            explicit ResidentPlatform(const bool addWall = false, const bool excludeWall = false) {
                PreparePhysics(addWall);
                PrepareCharacter(excludeWall);
            }

            void PreparePhysics(const bool addWall) {
                const auto owner = WorldDescriptor();
                const auto descriptor =
                    ControllerDescriptor({owner.sceneGeneration, WorldId(), owner.physicsWorld, 1, owner.originGeneration, 1});
                profile = descriptor.collisionProfile;
                channel = descriptor.queryChannel;
                layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value();
                Physics::ProjectCollisionSchema
                    authored{.defaultProfile = profile,
                             .layers = {{layer}},
                             .pairs = {{layer, layer, Physics::SimulationPairResponse::Block}},
                             .queryChannels = {{channel}},
                             .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}}}};
                auto normalized = Physics::NormalizedCollisionSchema::Create(authored).Value();
                runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value();
                physics = runtime
                              ->PrepareWorld(Physics::Test::SmallWorldSettings(),
                                             {std::make_shared<const Physics::NormalizedCollisionSchema>(std::move(normalized)), 1})
                              .Value();
                REQUIRE(physics->Activate(owner.physicsWorld).HasValue());
                shape = physics->CreateSceneShape(Physics::PhysicsBoxShape{{4, 0.25F, 4}}).Value();
                Physics::PhysicsBodyDescriptor platform;
                platform.shape = shape;
                platform.motion = Physics::PhysicsMotionType::Kinematic;
                Physics::PhysicsSceneCollisionBinding collision{.profile = profile,
                                                                .colliders = {
                                                                    {{}, Physics::PhysicsShapeSubresourceId::FromValue(11), {Material()}}}};
                body = physics->CreateSceneBody({.body = platform, .collision = std::move(collision)}).Value();
                if (addWall) {
                    wall = physics
                               ->CreateQueryFixture({.shape = Physics::PhysicsStaticPlaneShape{{1, 0, 0}, 0.5F},
                                                     .layer = layer,
                                                     .profile = profile,
                                                     .channel = channel})
                               .Value();
                }
                REQUIRE(physics
                            ->AdvanceFixedTick({.simulationTick = 1,
                                                .sceneGeneration = owner.sceneGeneration,
                                                .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
            }

            void PrepareCharacter(const bool excludeWall) {
                auto owner = WorldDescriptor();
                owner.collisionFilterGeneration = 1;
                owner.physicsSnapshotRevision = physics->PublishedTick().publicationRevision;
                character = CharacterWorld::Prepare(owner, Settings(1)).Value();
                auto descriptor = ControllerDescriptor(character->Descriptor());
                descriptor.capsule = {0.25F, 0.5F};
                descriptor.collisionRootPosition = {1, 1.02F, 0};
                descriptor.gravity = {};
                if (excludeWall)
                    descriptor.selectors.excludedBody = wall->body;
                controller = character->CreateController(descriptor).Value();
                REQUIRE(character->Activate().HasValue());
                adapter = std::make_unique<CharacterPhysicsQueryAdapter>(*physics);
                REQUIRE(character->SpawnController(controller, Query(0)).HasValue());
                REQUIRE(Tick(1).HasValue());
                REQUIRE(Snapshot().movement.groundBody == body);
                REQUIRE(Snapshot().movement.platformAttached);
                REQUIRE(Snapshot().movement.groundMaterial->asset == Material().asset);
            }

            CharacterPhysicsQueryContext Query(const std::uint64_t tick) {
                const auto &owner = character->Descriptor();
                return adapter->Context({owner.sceneGeneration, owner.identity, owner.physicsWorld, owner.collisionFilterGeneration,
                                         owner.originGeneration, tick, owner.physicsSnapshotRevision});
            }

            Result<void> Tick(const std::uint64_t tick, const std::span<const Physics::PhysicsKinematicTargetCommand> targets = {},
                              const Math::Vec3 intent = {}, const std::optional<Physics::PhysicsCapsuleShape> capsule = {}) {
                auto command = Movement(controller, tick, tick);
                command.desiredVelocityMetersPerSecond = intent;
                if (capsule)
                    command.shapeChange = CharacterShapeChangeRequest{*capsule};
                REQUIRE(character->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(tick);
                input.query = Query(tick);
                input.platformTargets = targets;
                return character->AdvanceFixedTick(input);
            }

            CharacterLocomotionSnapshot Snapshot() const {
                return character->ControllerLocomotionSnapshot(controller).Value();
            }

            Physics::PhysicsKinematicTargetCommand Target(const Physics::PhysicsPose pose) const {
                return {{Physics::PhysicsTransformAuthorityProtocolVersion, 2, character->Descriptor().sceneGeneration, body,
                         Physics::PhysicsCommandSourceId::Create(1).Value(), 2},
                        pose};
            }

            Physics::PhysicsQueryDescriptor FrameQuery() const {
                Physics::PhysicsQueryDescriptor query;
                query.world = physics->Identity();
                query.sceneGeneration = character->Descriptor().sceneGeneration;
                query.geometry = Physics::PhysicsCapsuleSweepQuery{{0.25F, 0.5F}, {1, 1.02F, 0}, {0, 1, 0}, {0, -1, 0}, 2};
                query.filter = {channel, Physics::PhysicsQueryTriggerPolicy::Exclude, layer, profile, {}, true};
                query.collection = Physics::PhysicsQueryCollection::All;
                query.maximumHitCount = Physics::MaximumPhysicsQueryHits;
                const auto source = physics->ReadSceneBodyReconciliation(body).Value().state.pose;
                query.bodyPoseFrame = Physics::PhysicsQueryBodyPoseFrame{body,
                                                                         shape,
                                                                         source,
                                                                         {{0, -0.5F, 0}, Math::Quaternion::Identity()},
                                                                         physics->PublishedTick().publicationRevision,
                                                                         2};
                return query;
            }
        };

        TEST_CASE("Resident Jolt platform descent rotation intent and ground share one predicted query frame",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            const auto source = scene.physics->ReadSceneBodyReconciliation(scene.body).Value().state;
            const auto publication = scene.physics->PublishedTick();
            const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            const std::array targets{scene.Target({{0, -0.5F, 0}, yaw})};
            REQUIRE(scene.Tick(2, targets, {0, 0, 3}).HasValue());
            const auto moved = scene.Snapshot();
            REQUIRE_FALSE(moved.movement.platformCarryRejected);
            REQUIRE(moved.movement.platformAttached);
            REQUIRE(moved.movement.groundBody == scene.body);
            REQUIRE(moved.transform.position.y == Catch::Approx(0.52F).margin(1.0e-4F));
            REQUIRE(moved.transform.position.x == Catch::Approx(0.0F).margin(1.0e-4F));
            REQUIRE(moved.transform.position.z == Catch::Approx(-0.95F).margin(1.0e-4F));
            REQUIRE(moved.movement.platformCarriedDisplacement.y == Catch::Approx(-0.5F).margin(1.0e-4F));
            REQUIRE(scene.physics->ReadSceneBodyReconciliation(scene.body).Value().state.pose == source.pose);
            REQUIRE(scene.physics->PublishedTick().completedTick == publication.completedTick);
            REQUIRE(scene.physics->PublishedTick().publicationRevision == publication.publicationRevision);
        }

        TEST_CASE("Native rising rotating platform transport does not collide with its own predicted support",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            const auto source = scene.physics->ReadSceneBodyReconciliation(scene.body).Value().state.pose;
            const auto revision = scene.physics->PublishedTick().publicationRevision;
            const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            const std::array targets{scene.Target({{0, 0.5F, 0}, yaw})};
            REQUIRE(scene.Tick(2, targets).HasValue());
            const auto moved = scene.Snapshot();
            REQUIRE_FALSE(moved.movement.platformCarryRejected);
            REQUIRE(moved.movement.platformAttached);
            REQUIRE(moved.transform.position.y == Catch::Approx(1.52F).margin(1.0e-4F));
            REQUIRE(moved.transform.position.z == Catch::Approx(-1.0F).margin(1.0e-4F));
            REQUIRE(scene.physics->ReadSceneBodyReconciliation(scene.body).Value().state.pose == source);
            REQUIRE(scene.physics->PublishedTick().publicationRevision == revision);
        }

        TEST_CASE("Rising rotating native support admits a shape change only after captured and transported clearance",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            const std::array targets{scene.Target({{0, 0.5F, 0}, yaw})};
            REQUIRE(scene.Tick(2, targets, {}, Physics::PhysicsCapsuleShape{0.25F, 0.75F}).HasValue());
            const auto moved = scene.Snapshot();
            REQUIRE(moved.movement.shapeChange);
            REQUIRE(moved.movement.shapeChange->status == CharacterShapeChangeStatus::Applied);
            REQUIRE(moved.movement.shapeChange->effectiveCapsule.cylindricalHalfHeightMeters == 0.75F);
            REQUIRE_FALSE(moved.movement.platformCarryRejected);
            REQUIRE(moved.movement.platformAttached);
            REQUIRE(moved.transform.position.y == Catch::Approx(1.77F).margin(1.0e-4F));
            const auto capsule = moved.movement.shapeChange->effectiveCapsule;
            REQUIRE(moved.transform.position.y - capsule.radiusMeters - capsule.cylindricalHalfHeightMeters ==
                    Catch::Approx(0.77F).margin(1.0e-4F));
            REQUIRE(moved.transform.position.z == Catch::Approx(-1.0F).margin(1.0e-4F));
        }

        TEST_CASE("Native carried shape changes retain captured ceilings and reject blocked transported endpoints",
                  "[physics][native][character][platform][frame]") {
            for (const bool atEndpoint : {false, true}) {
                ResidentPlatform scene;
                const Math::Vec3 ceiling = atEndpoint ? Math::Vec3{0, 2.9F, -1} : Math::Vec3{1.2F, 2.15F, 0};
                REQUIRE(scene.physics
                            ->CreateQueryFixture({.shape = Physics::PhysicsBoxShape{{0.1F, 0.25F, 0.1F}},
                                                  .pose = {ceiling, Math::Quaternion::Identity()},
                                                  .layer = scene.layer,
                                                  .profile = scene.profile,
                                                  .channel = scene.channel})
                            .HasValue());
                REQUIRE(
                    scene.character->RefreshPhysicsSnapshot(scene.physics->Identity(), scene.physics->PublishedTick().publicationRevision)
                        .HasValue());
                const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
                const std::array targets{scene.Target({{0, 0.5F, 0}, yaw})};
                REQUIRE(scene.Tick(2, targets, {}, Physics::PhysicsCapsuleShape{0.25F, 0.75F}).HasValue());
                const auto moved = scene.Snapshot();
                REQUIRE(moved.movement.shapeChange);
                REQUIRE(moved.movement.shapeChange->status == CharacterShapeChangeStatus::Blocked);
                REQUIRE(moved.movement.shapeChange->effectiveCapsule.cylindricalHalfHeightMeters == 0.5F);
                REQUIRE(moved.movement.platformAttached);
                REQUIRE(moved.transform.position.y == Catch::Approx(1.52F).margin(1.0e-4F));
            }
        }

        TEST_CASE("Predicted support queries preserve unrelated walls and explicit body exclusions",
                  "[physics][native][character][platform][frame]") {
            for (const bool excluded : {false, true}) {
                ResidentPlatform scene(true, excluded);
                const std::array targets{scene.Target({{-1, -0.5F, 0}, Math::Quaternion::Identity()})};
                REQUIRE(scene.Tick(2, targets).HasValue());
                const auto moved = scene.Snapshot();
                if (excluded) {
                    REQUIRE_FALSE(moved.movement.platformCarryRejected);
                    REQUIRE(moved.movement.platformAttached);
                    REQUIRE(moved.transform.position.x == Catch::Approx(0.0F).margin(1.0e-4F));
                } else {
                    REQUIRE(moved.movement.platformCarryRejected);
                    REQUIRE_FALSE(moved.movement.platformAttached);
                    REQUIRE(moved.transform.position.x > 0.7F);
                }
            }
        }

        TEST_CASE("Native body pose frames fence identity publication tick geometry selectors and bounds",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            const auto valid = scene.FrameQuery();
            std::array<Physics::PhysicsQueryHit, Physics::MaximumPhysicsQueryHits> hits;
            for (const int corruption : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
                auto query = valid;
                if (corruption == 0)
                    ++query.bodyPoseFrame->body.slot.generation;
                else if (corruption == 1)
                    ++query.bodyPoseFrame->shape.slot.generation;
                else if (corruption == 2)
                    ++query.bodyPoseFrame->sourcePublicationRevision;
                else if (corruption == 3)
                    ++query.bodyPoseFrame->consumingTick;
                else if (corruption == 4)
                    query.bodyPoseFrame->sourcePose.translation.x += 1;
                else if (corruption == 5)
                    query.bodyPoseFrame->predictedPose.translation.x = std::numeric_limits<float>::infinity();
                else if (corruption == 6)
                    query.geometry = Physics::PhysicsRayQuery{{}, {0, -1, 0}, 1};
                else if (corruption == 7)
                    query.filter.excludedBody = scene.body;
                else if (corruption == 8)
                    query.bodyPoseFrame->predictedPose.translation.x = Physics::MaximumPhysicsLocalHalfExtentMeters + 1;
                else if (corruption == 9)
                    query.filter.channel = Physics::PhysicsQueryChannelId::Parse("72345678-1234-4234-8234-123456789abc").Value();
                else
                    query.filter.requiredProfile = Physics::CollisionProfileId::Parse("82345678-1234-4234-8234-123456789abc").Value();
                REQUIRE(scene.physics->Query(query, hits).HasError());
            }
            REQUIRE(scene.physics->Query(valid, hits).Value().hitCount > 0);
            const auto before = Tests::AllocationProbe::Count();
            const auto result = scene.physics->Query(valid, hits);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(result.HasValue());
            REQUIRE(after == before);
        }

        TEST_CASE("Resident query projection keeps ambiguous authored compound material evidence absent",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            const std::array children{Physics::PhysicsSceneShapeInstance{scene.shape, {}},
                                      Physics::PhysicsSceneShapeInstance{scene.shape, {{0, -2, 0}, Math::Quaternion::Identity()}}};
            const auto compound = scene.physics->CreateSceneCompoundShape(children).Value();
            Physics::PhysicsSceneBodyDescriptor descriptor;
            descriptor.body.shape = compound;
            descriptor.body.pose.translation = {20, 0, 0};
            descriptor.collision =
                Physics::PhysicsSceneCollisionBinding{.profile = scene.profile,
                                                      .colliders = {{{}, Physics::PhysicsShapeSubresourceId::FromValue(21), {Material()}},
                                                                    {{}, Physics::PhysicsShapeSubresourceId::FromValue(22), {Material()}}}};
            const auto body = scene.physics->CreateSceneBody(descriptor).Value();
            auto query = scene.FrameQuery();
            query.bodyPoseFrame.reset();
            query.geometry = Physics::PhysicsRayQuery{{20, 5, 0}, {0, -1, 0}, 10};
            std::array<Physics::PhysicsQueryHit, Physics::MaximumPhysicsQueryHits> hits;
            const auto result = scene.physics->Query(query, hits).Value();
            REQUIRE(result.hitCount > 0);
            for (const auto &hit : std::span(hits).first(result.hitCount)) {
                REQUIRE(hit.body == body);
                REQUIRE(hit.shape == compound);
                REQUIRE_FALSE(hit.subshape);
                REQUIRE_FALSE(hit.material);
                REQUIRE(hit.filterSchemaGeneration == 1);
            }
        }

        TEST_CASE("Resident queries do not substitute default collision bindings or accept unknown fixture authority",
                  "[physics][native][character][platform][frame]") {
            ResidentPlatform scene;
            Physics::PhysicsSceneBodyDescriptor descriptor;
            descriptor.body.shape = scene.shape;
            descriptor.body.pose.translation = {20, 0, 0};
            const auto closed = scene.physics->CreateSceneBody(descriptor).Value();
            auto query = scene.FrameQuery();
            query.bodyPoseFrame.reset();
            query.geometry = Physics::PhysicsRayQuery{{20, 5, 0}, {0, -1, 0}, 10};
            std::array<Physics::PhysicsQueryHit, Physics::MaximumPhysicsQueryHits> hits;
            REQUIRE(scene.physics->Query(query, hits).Value().hitCount == 0);
            const auto pose = scene.physics->ReadSceneBodyReconciliation(closed).Value().state.pose;
            query.geometry = Physics::PhysicsCapsuleOverlapQuery{{0.25F, 0.5F}, {20, 1, 0}, {0, 1, 0}};
            query.bodyPoseFrame =
                Physics::PhysicsQueryBodyPoseFrame{closed, scene.shape, pose, pose, scene.physics->PublishedTick().publicationRevision, 2};
            RequireError(scene.physics->Query(query, hits), Physics::PhysicsErrors::CapabilityUnavailable);
            const auto unknown = Physics::CollisionProfileId::Parse("82345678-1234-4234-8234-123456789abc").Value();
            REQUIRE(
                scene.physics
                    ->CreateQueryFixture(
                        {.shape = Physics::PhysicsBoxShape{{1, 1, 1}}, .layer = scene.layer, .profile = unknown, .channel = scene.channel})
                    .HasError());
        }

#endif
    }  // namespace
}  // namespace Horo::Character
