#include "CharacterPlatformTestHelpers.h"

#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

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
                    const auto rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, 16'666'667.0F / 1'000'000'000.0F);
                    const auto offset = departure == 2 ? Math::Vec3{2, 0, 0} : rotation.Rotate({2, 0, 0});
                    const auto expected = policy == CharacterPlatformLeaveVelocityPolicy::None ? Math::Vec3{}
                                          : policy == CharacterPlatformLeaveVelocityPolicy::Linear
                                              ? Math::Vec3{3, 2, 0}
                                              : Math::Vec3{3, 2, 0} + Math::Cross(Math::Vec3{0, 1, 0}, offset);
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

        /** @brief Exercises a quarter-turn where start, transported and post-intent moment arms differ materially. */
        void RequireRotatingDeparture(const int departure) {
            PlatformScenario scenario(false, 64, false, CharacterPlatformLeaveVelocityPolicy::Point, false, {2, 0, 0});
            REQUIRE(scenario.Tick(1).HasValue());
            const auto yaw = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            const std::array targets{Target(scenario, 2, {{1, 0, 0}, yaw})};
            scenario.probe.groundPresent = departure == 2;
            const Math::Vec3 intent = departure == 1 ? Math::Vec3{0, 0, 3} : Math::Vec3{};
            REQUIRE(scenario.Tick(2, departure == 0, targets, departure == 2, intent).HasValue());
            const float seconds = 16'666'667.0F / 1'000'000'000.0F;
            const auto offset =
                departure == 2 ? Math::Vec3{2, 0, 0} : yaw.Rotate({2, 0, 0}) + (departure == 1 ? intent * seconds : Math::Vec3{});
            const auto expected = Math::Vec3{1.0F / seconds, 0, 0} + Math::Cross(Math::Vec3{0, Math::Pi * 0.5F / seconds, 0}, offset);
            const auto moved = scenario.Snapshot();
            RequireNear(moved.movement.platformLeaveVelocityMetersPerSecond, expected);
            REQUIRE_FALSE(moved.movement.platformAttached);
            if (departure == 2)
                RequireNear(moved.movement.platformCarriedDisplacement, {});
            else
                RequireNear(moved.movement.platformCarriedDisplacement, Math::Vec3{1, 0, 0} + yaw.Rotate({2, 0, 0}) - Math::Vec3{2, 0, 0});
        }

        TEST_CASE("Jump transfers point velocity at the transported quarter-turn attachment", "[physics][character][platform]") {
            RequireRotatingDeparture(0);
        }

        TEST_CASE("Walk off transfers point velocity at the actual post-intent departure position", "[physics][character][platform]") {
            RequireRotatingDeparture(1);
        }

        TEST_CASE("Explicit detach transfers point velocity before the admitted platform quarter-turn", "[physics][character][platform]") {
            RequireRotatingDeparture(2);
        }

        TEST_CASE("Blocked jump carry cannot leak platform transfer into displacement or gravity continuation",
                  "[physics][character][platform]") {
            PlatformScenario scenario(false, 64, false, CharacterPlatformLeaveVelocityPolicy::Point);
            REQUIRE(scenario.Tick(1).HasValue());
            scenario.probe.wall = true;
            scenario.probe.groundPresent = false;
            const std::array targets{Target(scenario, 2, {{1, 0, 0}, Math::Quaternion::Identity()})};
            REQUIRE(scenario.Tick(2, true, targets).HasValue());
            const auto moved = scenario.Snapshot();
            REQUIRE(moved.movement.platformCarryRejected);
            RequireNear(moved.movement.platformLeaveVelocityMetersPerSecond, {});
            REQUIRE(moved.movement.gravityVelocityMetersPerSecond.x == 0.0F);
            REQUIRE(moved.transform.position.x < 0.05F);
            REQUIRE_FALSE(moved.movement.platformAttachment);
            REQUIRE(scenario.Tick(3).HasValue());
            REQUIRE(scenario.Snapshot().movement.gravityVelocityMetersPerSecond.x == 0.0F);
            REQUIRE(scenario.Snapshot().transform.position.x == Catch::Approx(moved.transform.position.x));
        }

    }  // namespace
}  // namespace Horo::Character
