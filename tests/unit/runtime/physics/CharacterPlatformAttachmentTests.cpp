#include "AllocationProbe.h"
#include "CharacterPlatformTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>
#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

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

    }  // namespace
}  // namespace Horo::Character
