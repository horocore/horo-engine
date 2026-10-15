#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>
#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        /** @brief Supplies unobstructed placement without borrowing or modifying fixture state. */
        constexpr CharacterOverlapProbe UnobstructedPlatformSpace = [](void *, const CharacterOverlapProbeRequest &) noexcept {
            return Result<CharacterOverlapProbeResult>::Success({});
        };

        struct PlatformProbe final {
            CharacterSweepHit ground;
            CharacterPlatformBodyEvidence body;
            bool groundPresent{true};
            bool bodyPresent{true};
            bool fail{};
            bool wall{};
            CharacterWorld *shutdownWorld{};
            std::uint32_t bodyReads{};
            std::uint32_t failOnRead{};

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                const auto &probe = *static_cast<PlatformProbe *>(context);
                CharacterSweepProbeResult result;
                if (probe.groundPresent && request.direction.y < -0.5F && probe.ground.distanceMeters <= request.maximumDistanceMeters) {
                    result.hits[0] = probe.ground;
                    result.hitCount = 1;
                }
                if (probe.wall && request.direction.x > 0.5F) {
                    result.hits[0] = probe.ground;
                    result.hits[0].normal = {-1, 0, 0};
                    result.hits[0].distanceMeters = 0.05F;
                    result.hitCount = 1;
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            static Result<std::optional<CharacterPlatformBodyEvidence>> Body(void *context, Physics::BodyHandle, Physics::ShapeHandle,
                                                                             std::uint64_t) noexcept {
                auto &probe = *static_cast<PlatformProbe *>(context);
                ++probe.bodyReads;
                if (probe.shutdownWorld != nullptr)
                    probe.shutdownWorld->Shutdown();
                if (probe.fail || probe.bodyReads == probe.failOnRead)
                    return Result<std::optional<CharacterPlatformBodyEvidence>>::Failure(
                        MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
                return Result<std::optional<CharacterPlatformBodyEvidence>>::Success(probe.bodyPresent ? std::optional{probe.body}
                                                                                                       : std::nullopt);
            }

            CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        UnobstructedPlatformSpace,
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Sweep,
                        Body};
            }
        };

        struct PlatformScenario final {
            std::unique_ptr<CharacterWorld> world;
            CharacterControllerHandle controller;
            CharacterControllerHandle secondController;
            PlatformProbe probe;

            explicit PlatformScenario(const bool dynamicAllowed = false, const std::uint32_t queries = 64,
                                      const bool twoControllers = false,
                                      const CharacterPlatformLeaveVelocityPolicy policy = CharacterPlatformLeaveVelocityPolicy::None,
                                      const bool inheritHeading = false, const Math::Vec3 root = {}) {
                auto settings = Settings().Values();
                settings.work.maximumQueriesPerTick = queries;
                world = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value();
                auto descriptor = ControllerDescriptor(world->Descriptor());
                descriptor.gravity = {};
                descriptor.platformLeaveVelocity = policy;
                descriptor.inheritPlatformHeading = inheritHeading;
                descriptor.collisionRootPosition = root;
                descriptor.allowDynamicPlatformAttachment = dynamicAllowed;
                controller = world->CreateController(descriptor).Value();
                if (twoControllers)
                    secondController = world->CreateController(descriptor).Value();
                ActivateControllers(twoControllers);
                const Physics::BodyHandle body{world->Descriptor().physicsWorld, {4, 2}};
                const Physics::ShapeHandle shape{world->Descriptor().physicsWorld, {5, 3}};
                probe.ground = {.body = body, .shape = shape, .point = {2, 0, 1}, .normal = {0, 1, 0}, .distanceMeters = 0.02F};
                probe.body = {body, shape, Physics::PhysicsMotionType::Kinematic, {}};
            }

            void ActivateControllers(const bool twoControllers) {
                REQUIRE(world->Activate().HasValue());
                const std::array controllers{controller, secondController};
                for (std::size_t index = 0; index < (twoControllers ? 2U : 1U); ++index) {
                    const auto &descriptor = world->Descriptor();
                    CharacterPhysicsQueryContext query{descriptor.sceneGeneration,        descriptor.identity,
                                                       descriptor.physicsWorld,           nullptr,
                                                       UnobstructedPlatformSpace,         descriptor.collisionFilterGeneration,
                                                       descriptor.originGeneration,       0,
                                                       descriptor.physicsSnapshotRevision};
                    REQUIRE(world->SpawnController(controllers[index], query).HasValue());
                }
            }

            CharacterFixedTickInput QueuedTick(const std::uint64_t tick) {
                REQUIRE(world->QueueMovementCommand(Movement(controller, tick, tick)).HasValue());
                auto input = FixedTick(tick);
                input.query = probe.Context(world->Descriptor(), tick);
                return input;
            }

            Result<void> Tick(const std::uint64_t tick, const bool jump = false,
                              const std::span<const Physics::PhysicsKinematicTargetCommand> targets = {}, const bool detach = false,
                              const Math::Vec3 velocity = {}) {
                auto command = Movement(controller, tick, tick);
                command.jumpRequested = jump;
                command.detachPlatformRequested = detach;
                command.desiredVelocityMetersPerSecond = velocity;
                REQUIRE(world->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(tick);
                input.query = probe.Context(world->Descriptor(), tick);
                input.platformTargets = targets;
                return world->AdvanceFixedTick(input);
            }

            CharacterLocomotionSnapshot Snapshot() const {
                return world->ControllerLocomotionSnapshot(controller).Value();
            }
        };

        void RequireNear(const Math::Vec3 actual, const Math::Vec3 expected) {
            REQUIRE(actual.x == Catch::Approx(expected.x).margin(1.0e-5F));
            REQUIRE(actual.y == Catch::Approx(expected.y).margin(1.0e-5F));
            REQUIRE(actual.z == Catch::Approx(expected.z).margin(1.0e-5F));
        }

        TEST_CASE("Character fixed ticks own an attachment frame independent of copied query storage", "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const auto saved = scenario.Snapshot();
            REQUIRE(saved.movement.platformAttached);
            REQUIRE(saved.transform.platformAttached);
            const auto attachment = saved.movement.platformAttachment.value();
            REQUIRE(attachment.body == scenario.probe.body.body);
            REQUIRE(attachment.shape == scenario.probe.body.shape);
            REQUIRE(attachment.sourceTick == 1);
            REQUIRE(attachment.physicsSnapshotRevision == scenario.world->Descriptor().physicsSnapshotRevision);
            REQUIRE(saved.movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Attached);
            const auto &worldDescriptor = scenario.world->Descriptor();
            const auto debug =
                scenario.world->CaptureDebugSnapshot({scenario.controller, worldDescriptor.physicsWorld,
                                                      worldDescriptor.collisionFilterGeneration, worldDescriptor.originGeneration});
            REQUIRE(debug.snapshot.has_value());
            REQUIRE(debug.snapshot->Locomotion()->movement.platformAttachment->body == attachment.body);
            REQUIRE(debug.snapshot->Locomotion()->movement.platformAttachment->shape == attachment.shape);
            REQUIRE(debug.snapshot->Locomotion()->movement.platformAttachment->sourceTick == attachment.sourceTick);
            REQUIRE(debug.snapshot->Transform().publicationRevision == saved.transform.publicationRevision);
            RequireNear(attachment.localContactPoint, {2, 0, 1});
            scenario.probe.ground.point = {8, 0, 7};
            REQUIRE(saved.movement.platformAttachment->localContactPoint == attachment.localContactPoint);
            REQUIRE(scenario.Tick(2).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::None);
            REQUIRE(scenario.probe.bodyReads == 3);
        }

        TEST_CASE("Rotating support transports the root while preserving body local contact and Character up",
                  "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const auto previous = scenario.Snapshot();
            const auto local = previous.movement.platformAttachment.value();
            const std::array rotations{Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F),
                                       Math::Quaternion::FromAxisAngle({1, 0, 0}, Math::Pi / 9.0F)};
            std::uint64_t tick = 2;
            for (const auto rotation : rotations) {
                scenario.probe.body.pose = {{3, 0, -2}, rotation};
                scenario.probe.ground.point = scenario.probe.body.pose.translation + rotation.Rotate(local.localContactPoint);
                scenario.probe.ground.normal = rotation.Rotate(local.localContactNormal);
                REQUIRE(scenario.Tick(tick++).HasValue());
                const auto rotated = scenario.Snapshot();
                RequireNear(rotated.movement.platformAttachment->localContactPoint, local.localContactPoint);
                RequireNear(rotated.movement.platformAttachment->localContactNormal, local.localContactNormal);
                RequireNear(rotated.transform.position,
                            scenario.probe.body.pose.translation + rotation.Rotate(local.localRoot.translation));
                REQUIRE(rotated.transform.heading == previous.transform.heading);
                REQUIRE(rotated.transform.up == previous.transform.up);
                REQUIRE(rotated.movement.platformAttachmentChange == CharacterPlatformAttachmentChange::None);
            }
        }

        TEST_CASE("Movement base changes include body shape and authored child generations", "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            ++scenario.probe.ground.body->slot.generation;
            scenario.probe.body.body = *scenario.probe.ground.body;
            REQUIRE(scenario.Tick(2).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::BaseChanged);
            ++scenario.probe.ground.shape.slot.generation;
            scenario.probe.body.shape = scenario.probe.ground.shape;
            REQUIRE(scenario.Tick(3).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::BaseChanged);
            scenario.probe.ground.subshape = Physics::PhysicsShapeSubresourceId::FromValue(8);
            REQUIRE(scenario.Tick(4).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::BaseChanged);
            REQUIRE(scenario.Snapshot().movement.platformAttachment->subshape == scenario.probe.ground.subshape);
            scenario.probe.ground.subshape = Physics::PhysicsShapeSubresourceId::FromValue(9);
            REQUIRE(scenario.Tick(5).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::BaseChanged);
            scenario.probe.bodyPresent = false;
            REQUIRE(scenario.Tick(6).HasValue());
            REQUIRE_FALSE(scenario.Snapshot().movement.platformAttachment);
            REQUIRE_FALSE(scenario.Snapshot().transform.platformAttached);
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Stale);
        }

        TEST_CASE("Attachments require eligible body backed blocking walkable support", "[physics][character][platform]") {
            for (const int mode : {0, 1, 2, 3}) {
                PlatformScenario scenario;
                if (mode == 0)
                    scenario.probe.ground.trigger = true;
                else if (mode == 1)
                    scenario.probe.ground.response = Physics::PhysicsQueryResponse::Overlap;
                else if (mode == 2)
                    scenario.probe.ground.normal = Math::Normalize(Math::Vec3{1, 0.1F, 0});
                else
                    scenario.probe.ground.body.reset();
                REQUIRE(scenario.Tick(1).HasValue());
                REQUIRE_FALSE(scenario.Snapshot().movement.platformAttachment);
                REQUIRE(scenario.probe.bodyReads == 0);
            }
            PlatformScenario denied;
            denied.probe.body.motion = Physics::PhysicsMotionType::Dynamic;
            REQUIRE(denied.Tick(1).HasValue());
            REQUIRE_FALSE(denied.Snapshot().movement.platformAttachment);
            PlatformScenario allowed(true);
            allowed.probe.body.motion = Physics::PhysicsMotionType::Dynamic;
            REQUIRE(allowed.Tick(1).HasValue());
            REQUIRE(allowed.Snapshot().movement.platformAttachment.has_value());
            REQUIRE(allowed.Snapshot().movement.platformAttachment->motion == Physics::PhysicsMotionType::Dynamic);
        }

        TEST_CASE("Walk off jump and teleport detach the owned base without retaining a query borrow", "[physics][character][platform]") {
            for (const bool jump : {false, true}) {
                PlatformScenario scenario;
                REQUIRE(scenario.Tick(1).HasValue());
                scenario.probe.groundPresent = false;
                REQUIRE(scenario.Tick(2, jump).HasValue());
                REQUIRE_FALSE(scenario.Snapshot().movement.platformAttachment);
                REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Detached);
            }
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const auto saved = scenario.Snapshot();
            OverlapProbe overlap;
            REQUIRE(
                scenario.world->TeleportController({scenario.controller, 2, {4, 1, 3}}, overlap.Context(scenario.world->Descriptor(), 2))
                    .HasValue());
            REQUIRE_FALSE(scenario.world->ControllerTransform(scenario.controller).Value().platformAttached);
            REQUIRE(scenario.world->ControllerLocomotionSnapshot(scenario.controller).HasError());
            scenario.world->Shutdown();
            REQUIRE(saved.movement.platformAttachment.has_value());
            REQUIRE(ValidateCharacterLocomotionSnapshot(saved, ControllerDescriptor(scenario.world->Descriptor())).HasValue());
        }

        TEST_CASE("Static support attaches but absent pose providers never fabricate a base", "[physics][character][platform]") {
            PlatformScenario scenario;
            scenario.probe.body.motion = Physics::PhysicsMotionType::Static;
            REQUIRE(scenario.Tick(1).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachment->motion == Physics::PhysicsMotionType::Static);
            auto input = scenario.QueuedTick(2);
            input.query.platformBody = nullptr;
            REQUIRE(scenario.world->AdvanceFixedTick(input).HasValue());
            const auto detached = scenario.Snapshot();
            REQUIRE(detached.movement.grounded);
            REQUIRE_FALSE(detached.movement.platformAttached);
            REQUIRE_FALSE(detached.movement.platformAttachment);
            REQUIRE(detached.movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Unavailable);
            REQUIRE(scenario.probe.bodyReads == 1);
            REQUIRE(scenario.Tick(3).HasValue());
            REQUIRE(scenario.Snapshot().movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Attached);
        }

        TEST_CASE("Copied attachment validators reject corrupted identity frames and lifecycle outcomes",
                  "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const auto saved = scenario.Snapshot();
            const auto descriptor = ControllerDescriptor(scenario.world->Descriptor());
            for (const int corruption : {0, 1, 2, 3, 4, 5}) {
                auto invalid = saved;
                auto &movement = invalid.movement;
                auto &attachment = *movement.platformAttachment;
                if (corruption == 0)
                    ++attachment.body.slot.generation;
                else if (corruption == 1)
                    ++attachment.shape.slot.generation;
                else if (corruption == 2)
                    attachment.localRoot.translation.x += 1;
                else if (corruption == 3)
                    attachment.localContactNormal = {};
                else if (corruption == 4)
                    ++attachment.sourceTick;
                else
                    movement.platformAttachmentChange = CharacterPlatformAttachmentChange::Detached;
                REQUIRE(ValidateCharacterLocomotionSnapshot(invalid, descriptor).HasError());
            }
            REQUIRE(ValidateCharacterLocomotionSnapshot(saved, descriptor).HasValue());
        }

        TEST_CASE("A Character world replacement cannot inherit a retired world's attachment", "[physics][character][platform]") {
            PlatformScenario retired;
            REQUIRE(retired.Tick(1).HasValue());
            const auto historical = retired.Snapshot();
            retired.world->Shutdown();
            auto replacementDescriptor = WorldDescriptor();
            ++replacementDescriptor.sceneGeneration;
            auto replacement = CharacterWorld::Prepare(replacementDescriptor, Settings()).Value();
            const auto publishedDescriptor = replacement->Descriptor();
            const auto controller = replacement->CreateController(ControllerDescriptor(publishedDescriptor)).Value();
            REQUIRE(replacement->Activate().HasValue());
            OverlapProbe overlap;
            REQUIRE(replacement->SpawnController(controller, overlap.Context(publishedDescriptor)).HasValue());
            REQUIRE_FALSE(replacement->ControllerTransform(controller).Value().platformAttached);
            REQUIRE(replacement->ControllerLocomotionSnapshot(controller).HasError());
            REQUIRE(historical.movement.platformAttachment.has_value());
            REQUIRE(ValidateCharacterLocomotionSnapshot(historical, ControllerDescriptor(publishedDescriptor)).HasError());
        }

        TEST_CASE("Invalid platform evidence and exhausted body read budgets preserve the prior committed attachment",
                  "[physics][character][platform]") {
            for (const int failure : {0, 1, 2, 3}) {
                PlatformScenario scenario;
                REQUIRE(scenario.Tick(1).HasValue());
                const auto previous = scenario.Snapshot();
                if (failure == 0)
                    scenario.probe.fail = true;
                else if (failure == 1)
                    scenario.probe.body.pose.rotation.w = 2;
                else if (failure == 2)
                    scenario.probe.body.pose.translation.x = std::numeric_limits<float>::quiet_NaN();
                else
                    ++scenario.probe.body.shape.slot.generation;
                REQUIRE(scenario.Tick(2).HasError());
                const auto retained = scenario.Snapshot();
                REQUIRE(retained.stateRevision == previous.stateRevision);
                REQUIRE(retained.movement.platformAttachment->sourceTick == 1);
                REQUIRE(retained.transform.position == previous.transform.position);
            }
            PlatformScenario bounded(false, 1);
            RequireError(bounded.Tick(1), CharacterErrors::CapacityExceeded);
            REQUIRE(bounded.probe.bodyReads == 0);
            REQUIRE(bounded.world->ControllerLocomotionSnapshot(bounded.controller).HasError());
        }

        TEST_CASE("Attachment read shutdown publishes no candidate and steady ticks allocate no storage",
                  "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            auto input = scenario.QueuedTick(2);
            const auto before = Tests::AllocationProbe::Count();
            const auto moved = scenario.world->AdvanceFixedTick(input);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(moved.HasValue());
            REQUIRE(after == before);
            scenario.probe.shutdownWorld = scenario.world.get();
            RequireError(scenario.Tick(3), CharacterErrors::InvalidState);
            REQUIRE(scenario.world->ActiveControllerCount() == 0);
            REQUIRE(scenario.world->ControllerLocomotionSnapshot(scenario.controller).HasError());
        }

        TEST_CASE("A later controller's platform read failure rolls back every staged attachment in the tick",
                  "[physics][character][platform]") {
            PlatformScenario scenario(false, 64, true);
            const auto second = scenario.secondController;
            REQUIRE(scenario.world->QueueMovementCommand(Movement(second, 1, 1)).HasValue());
            REQUIRE(scenario.Tick(1).HasValue());
            const auto firstBefore = scenario.Snapshot();
            const auto secondBefore = scenario.world->ControllerLocomotionSnapshot(second).Value();
            const auto &descriptor = scenario.world->Descriptor();
            const CharacterDebugCaptureRequest debugRequest{scenario.controller, descriptor.physicsWorld,
                                                            descriptor.collisionFilterGeneration, descriptor.originGeneration};
            const auto debugBefore = scenario.world->CaptureDebugSnapshot(debugRequest);
            REQUIRE(debugBefore.snapshot.has_value());
            REQUIRE(scenario.probe.bodyReads == 2);
            scenario.probe.failOnRead = 6;
            REQUIRE(scenario.world->QueueMovementCommand(Movement(second, 2, 2)).HasValue());
            REQUIRE(scenario.Tick(2).HasError());
            REQUIRE(scenario.probe.bodyReads == 6);
            const auto firstAfter = scenario.Snapshot();
            const auto secondAfter = scenario.world->ControllerLocomotionSnapshot(second).Value();
            REQUIRE(firstAfter.stateRevision == firstBefore.stateRevision);
            REQUIRE(secondAfter.stateRevision == secondBefore.stateRevision);
            REQUIRE(firstAfter.movement.platformAttachment->sourceTick == 1);
            REQUIRE(secondAfter.movement.platformAttachment->sourceTick == 1);
            const auto debugAfter = scenario.world->CaptureDebugSnapshot(debugRequest);
            REQUIRE(debugAfter.snapshot.has_value());
            REQUIRE(debugAfter.snapshot->Identity().sourceTick == debugBefore.snapshot->Identity().sourceTick);
            REQUIRE(debugAfter.snapshot->Probes().size() == debugBefore.snapshot->Probes().size());
            REQUIRE(debugAfter.snapshot->Probes()[0].position == debugBefore.snapshot->Probes()[0].position);
            REQUIRE(debugAfter.snapshot->Locomotion()->movement.platformAttachment->sourceTick == 1);
        }

        Physics::PhysicsKinematicTargetCommand Target(const PlatformScenario &scenario, const std::uint64_t tick,
                                                      const Physics::PhysicsPose pose) {
            return {{Physics::PhysicsTransformAuthorityProtocolVersion, tick, scenario.world->Descriptor().sceneGeneration,
                     scenario.probe.body.body, Physics::PhysicsCommandSourceId::Create(7).Value(), tick},
                    pose};
        }

        TEST_CASE("Kinematic carry precedes intent and is not replayed on the following fixed tick", "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const std::array first{Target(scenario, 2, {{1, 0, 0}, Math::Quaternion::Identity()})};
            REQUIRE(scenario.Tick(2, false, first, false, {3, 0, 0}).HasValue());
            const auto moved = scenario.Snapshot();
            RequireNear(moved.movement.platformCarriedDisplacement, {1, 0, 0});
            RequireNear(moved.transform.position, {1.05F, 0, 0});
            RequireNear(moved.movement.achievedVelocityMetersPerSecond, {63, 0, 0});
            REQUIRE_FALSE(moved.movement.platformCarryRejected);
            scenario.probe.body.pose = first[0].targetPose;
            const std::array second{Target(scenario, 3, {{2, 0, 0}, Math::Quaternion::Identity()})};
            REQUIRE(scenario.Tick(3, false, second).HasValue());
            RequireNear(scenario.Snapshot().movement.platformCarriedDisplacement, {1, 0, 0});
            RequireNear(scenario.Snapshot().transform.position, {2.05F, 0, 0});
        }

        TEST_CASE("Full platform rotation transports an offset root but only explicit twist changes heading",
                  "[physics][character][platform]") {
            for (const bool inherit : {false, true}) {
                PlatformScenario scenario(false, 64, false, CharacterPlatformLeaveVelocityPolicy::None, inherit, {2, 0, 0});
                REQUIRE(scenario.Tick(1).HasValue());
                const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
                const std::array targets{Target(scenario, 2, {{}, yaw})};
                REQUIRE(scenario.Tick(2, false, targets).HasValue());
                const auto moved = scenario.Snapshot();
                RequireNear(moved.transform.position, yaw.Rotate({2, 0, 0}));
                RequireNear(moved.transform.up, {0, 1, 0});
                RequireNear(moved.transform.heading.Rotate({0, 0, 1}), inherit ? yaw.Rotate({0, 0, 1}) : Math::Vec3{0, 0, 1});
            }
        }

        TEST_CASE("Dynamic platform prediction integrates copied linear and angular motion for exactly one quantum",
                  "[physics][character][platform]") {
            PlatformScenario scenario(true, 64, false, CharacterPlatformLeaveVelocityPolicy::None, false, {2, 0, 0});
            scenario.probe.body.motion = Physics::PhysicsMotionType::Dynamic;
            REQUIRE(scenario.Tick(1).HasValue());
            scenario.probe.body.linearVelocityMetersPerSecond = {3, 0, 0};
            scenario.probe.body.angularVelocityRadiansPerSecond = {0, Math::Pi * 30.0F, 0};
            REQUIRE(scenario.Tick(2).HasValue());
            const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            RequireNear(scenario.Snapshot().transform.position, Math::Vec3{0.05F, 0, 0} + yaw.Rotate({2, 0, 0}));
        }

        TEST_CASE("Jump walk off and forced detach transfer only the configured support velocity once", "[physics][character][platform]") {
            for (const auto policy : {CharacterPlatformLeaveVelocityPolicy::None, CharacterPlatformLeaveVelocityPolicy::Linear,
                                      CharacterPlatformLeaveVelocityPolicy::Point}) {
                for (const int departure : {0, 1, 2}) {
                    PlatformScenario scenario(true, 64, false, policy, false, {2, 0, 0});
                    scenario.probe.body.motion = Physics::PhysicsMotionType::Dynamic;
                    REQUIRE(scenario.Tick(1).HasValue());
                    scenario.probe.body.linearVelocityMetersPerSecond = {3, 2, 0};
                    scenario.probe.body.angularVelocityRadiansPerSecond = {0, 1, 0};
                    scenario.probe.groundPresent = departure == 2;
                    REQUIRE(scenario.Tick(2, departure == 0, {}, departure == 2).HasValue());
                    const auto moved = scenario.Snapshot();
                    const auto expected = policy == CharacterPlatformLeaveVelocityPolicy::None     ? Math::Vec3{}
                                          : policy == CharacterPlatformLeaveVelocityPolicy::Linear ? Math::Vec3{3, 2, 0}
                                                                                                   : Math::Vec3{3, 2, -2};
                    RequireNear(moved.movement.platformLeaveVelocityMetersPerSecond, expected);
                    REQUIRE_FALSE(moved.movement.platformAttachment);
                    REQUIRE_FALSE(moved.transform.grounded);
                    scenario.probe.groundPresent = false;
                    REQUIRE(scenario.Tick(3).HasValue());
                    RequireNear(scenario.Snapshot().movement.platformLeaveVelocityMetersPerSecond, {});
                    REQUIRE(scenario.Snapshot().movement.gravityVelocityMetersPerSecond.x == Catch::Approx(expected.x));
                }
            }
        }

        TEST_CASE("Blocked carry is swept and detaches rather than transporting through a wall", "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            scenario.probe.wall = true;
            const std::array targets{Target(scenario, 2, {{1, 0, 0}, Math::Quaternion::Identity()})};
            REQUIRE(scenario.Tick(2, false, targets).HasValue());
            const auto moved = scenario.Snapshot();
            REQUIRE(moved.movement.platformCarryRejected);
            REQUIRE(moved.transform.position.x < 0.05F);
            REQUIRE_FALSE(moved.movement.platformAttached);
            REQUIRE(moved.movement.platformAttachmentChange == CharacterPlatformAttachmentChange::Detached);
        }

        TEST_CASE("Malformed stale and duplicate platform targets fail before publishing a carried candidate",
                  "[physics][character][platform]") {
            for (const int failure : {0, 1, 2, 3}) {
                PlatformScenario scenario;
                REQUIRE(scenario.Tick(1).HasValue());
                const auto saved = scenario.Snapshot();
                std::array targets{Target(scenario, 2, {{1, 0, 0}, Math::Quaternion::Identity()}),
                                   Target(scenario, 2, {{1, 0, 0}, Math::Quaternion::Identity()})};
                std::size_t count = 1;
                if (failure == 0)
                    ++targets[0].identity.sceneGeneration;
                else if (failure == 1)
                    targets[0].identity.simulationTick = 1;
                else if (failure == 2)
                    targets[0].targetPose.translation.x = std::numeric_limits<float>::infinity();
                else
                    count = 2;
                REQUIRE(scenario.Tick(2, false, std::span{targets}.first(count)).HasError());
                REQUIRE(scenario.Snapshot().stateRevision == saved.stateRevision);
                REQUIRE(scenario.Snapshot().transform.position == saved.transform.position);
            }
        }

        TEST_CASE("Platform carry rejects nonfinite velocity and oversized motion without altering committed state",
                  "[physics][character][platform]") {
            for (const bool invalidVelocity : {false, true}) {
                PlatformScenario scenario(true);
                scenario.probe.body.motion = Physics::PhysicsMotionType::Dynamic;
                REQUIRE(scenario.Tick(1).HasValue());
                const auto saved = scenario.Snapshot();
                scenario.probe.body.linearVelocityMetersPerSecond = {invalidVelocity ? std::numeric_limits<float>::quiet_NaN() : 1.0e8F, 0,
                                                                     0};
                REQUIRE(scenario.Tick(2).HasError());
                REQUIRE(scenario.Snapshot().stateRevision == saved.stateRevision);
            }
        }

        TEST_CASE("Moving attachment without a query provider fails explicitly and retains its committed state",
                  "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            const auto saved = scenario.Snapshot();
            auto input = scenario.QueuedTick(2);
            input.query.platformBody = nullptr;
            RequireError(scenario.world->AdvanceFixedTick(input), CharacterErrors::OperationUnsupported);
            REQUIRE(scenario.Snapshot().stateRevision == saved.stateRevision);
            REQUIRE(scenario.Snapshot().movement.platformAttachment->sourceTick == 1);
        }

        TEST_CASE("Unknown leave policy and nonfinite copied motion diagnostics are rejected", "[physics][character][platform]") {
            PlatformScenario scenario;
            REQUIRE(scenario.Tick(1).HasValue());
            auto descriptor = ControllerDescriptor(scenario.world->Descriptor());
            descriptor.platformLeaveVelocity = static_cast<CharacterPlatformLeaveVelocityPolicy>(255);
            REQUIRE(ValidateCharacterControllerDescriptor(descriptor).HasError());
            descriptor.platformLeaveVelocity = CharacterPlatformLeaveVelocityPolicy::None;
            auto snapshot = scenario.Snapshot();
            snapshot.movement.platformCarriedDisplacement.x = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasError());
            snapshot = scenario.Snapshot();
            snapshot.movement.platformLeaveVelocityMetersPerSecond.x = std::numeric_limits<float>::infinity();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasError());
        }

    }  // namespace
}  // namespace Horo::Character
