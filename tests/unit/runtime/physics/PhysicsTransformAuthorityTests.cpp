#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTransformAuthority.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] PhysicsWorldId World(const std::uint64_t value = 71) {
            return PhysicsWorldId::Create(value).Value();
        }

        [[nodiscard]] BodyHandle Body(const std::uint32_t index = 1, const std::uint32_t generation = 1) {
            return {World(), {index, generation}};
        }

        [[nodiscard]] PhysicsPose Pose(const float x) {
            return {.translation = {x, 0, 0}, .rotation = Math::Quaternion::Identity()};
        }

        [[nodiscard]] PhysicsTransformCommandIdentity Identity(const BodyHandle body, const std::uint64_t tick = 1,
                                                               const std::uint64_t sequence = 1, const std::uint64_t source = 4) {
            return {.simulationTick = tick,
                    .sceneGeneration = 9,
                    .body = body,
                    .source = PhysicsCommandSourceId::Create(source).Value(),
                    .sourceSequence = sequence};
        }

        [[nodiscard]] PhysicsStaticTransformCommand StaticCommand(const BodyHandle body, const std::uint64_t tick, const float x,
                                                                  const PhysicsStaticTransformUpdatePolicy policy,
                                                                  const std::uint64_t sequence = 1) {
            return {.identity = Identity(body, tick, sequence), .authoredPose = Pose(x), .updatePolicy = policy};
        }

        [[nodiscard]] PhysicsKinematicTargetCommand KinematicCommand(const BodyHandle body, const std::uint64_t tick, const float x,
                                                                     const std::uint64_t sequence = 1) {
            return {.identity = Identity(body, tick, sequence), .targetPose = Pose(x)};
        }

        [[nodiscard]] PhysicsDynamicTransformCommand DynamicCommand(const BodyHandle body, const std::uint64_t tick, const float x,
                                                                    const PhysicsDynamicTransformOperation operation,
                                                                    const PhysicsTeleportVelocityPolicy velocityPolicy,
                                                                    const std::uint64_t sequence = 1) {
            return {.identity = Identity(body, tick, sequence),
                    .targetPose = Pose(x),
                    .operation = operation,
                    .velocityPolicy = velocityPolicy};
        }

        [[nodiscard]] PhysicsDynamicTransformSnapshot Snapshot(const BodyHandle body, const std::uint64_t tick, const float x,
                                                               const Math::Vec3 linearVelocity = {}) {
            return {.completedTick = tick,
                    .sceneGeneration = 9,
                    .state = {.body = body,
                              .pose = Pose(x),
                              .linearVelocity = linearVelocity,
                              .angularVelocity = {},
                              .activity = PhysicsBodyActivity::Awake}};
        }

        [[nodiscard]] std::unique_ptr<PhysicsBodyTransformAuthority> Prepared(const std::uint32_t maximumBodies = 4,
                                                                              const std::uint32_t maximumCommands = 8) {
            return PhysicsBodyTransformAuthority::Prepare(
                       {.world = World(), .sceneGeneration = 9, .maximumBodies = maximumBodies, .maximumPendingCommands = maximumCommands})
                .Value();
        }

        template <typename ResultType> void RequireCode(const ResultType &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        void Register(PhysicsBodyTransformAuthority &authority, const BodyHandle body, const PhysicsMotionType motion,
                      const float authoredX = 0) {
            REQUIRE(authority.RegisterBody({.body = body, .motion = motion, .authoredPose = Pose(authoredX)}).HasValue());
        }
    }  // namespace

    TEST_CASE("Physics transform authority maps body modes and validates typed command evidence", "[physics][transform][contract]") {
        REQUIRE(ResolvePhysicsTransformAuthority(PhysicsMotionType::Static).Value() == PhysicsTransformAuthority::StaticScene);
        REQUIRE(ResolvePhysicsTransformAuthority(PhysicsMotionType::Kinematic).Value() == PhysicsTransformAuthority::KinematicTarget);
        REQUIRE(ResolvePhysicsTransformAuthority(PhysicsMotionType::Dynamic).Value() == PhysicsTransformAuthority::DynamicSolver);
        RequireCode(ResolvePhysicsTransformAuthority(static_cast<PhysicsMotionType>(255)), PhysicsErrors::OperationUnsupported);

        const auto body = Body();
        REQUIRE(ValidatePhysicsStaticTransformCommand(StaticCommand(body, 1, 2, PhysicsStaticTransformUpdatePolicy::Rebuild), World(), 9, 1)
                    .HasValue());
        REQUIRE(ValidatePhysicsKinematicTargetCommand(KinematicCommand(body, 1, 2), World(), 9, 1).HasValue());
        REQUIRE(ValidatePhysicsDynamicTransformCommand(DynamicCommand(body, 1, 2, PhysicsDynamicTransformOperation::Teleport,
                                                                      PhysicsTeleportVelocityPolicy::Preserve),
                                                       World(), 9, 1)
                    .HasValue());

        auto invalid = KinematicCommand(body, 1, 2);
        invalid.identity.sourceSequence = 0;
        RequireCode(ValidatePhysicsKinematicTargetCommand(invalid, World(), 9, 1), PhysicsErrors::CommandOrderInvalid);
        invalid = KinematicCommand(body, 1, 2);
        invalid.targetPose.rotation.w = 2;
        RequireCode(ValidatePhysicsKinematicTargetCommand(invalid, World(), 9, 1), PhysicsErrors::DescriptorInvalid);
        invalid = KinematicCommand(body, 1, 2);
        invalid.identity.body.world = World(72);
        RequireCode(ValidatePhysicsKinematicTargetCommand(invalid, World(), 9, 1), PhysicsErrors::HandleWorldMismatch);
        invalid = KinematicCommand(body, 2, 2);
        RequireCode(ValidatePhysicsKinematicTargetCommand(invalid, World(), 9, 1), PhysicsErrors::CommandOrderInvalid);

        auto unknownPolicy = StaticCommand(body, 1, 2, PhysicsStaticTransformUpdatePolicy::Rebuild);
        unknownPolicy.updatePolicy = static_cast<PhysicsStaticTransformUpdatePolicy>(255);
        RequireCode(ValidatePhysicsStaticTransformCommand(unknownPolicy, World(), 9, 1), PhysicsErrors::OperationUnsupported);
        auto unknownOperation =
            DynamicCommand(body, 1, 2, PhysicsDynamicTransformOperation::Teleport, PhysicsTeleportVelocityPolicy::Preserve);
        unknownOperation.operation = static_cast<PhysicsDynamicTransformOperation>(255);
        RequireCode(ValidatePhysicsDynamicTransformCommand(unknownOperation, World(), 9, 1), PhysicsErrors::OperationUnsupported);
    }

    TEST_CASE("Physics transform authority rejects direct host writes and conflicting command modes", "[physics][transform][authority]") {
        auto authority = Prepared();
        const auto staticBody = Body(1);
        const auto kinematicBody = Body(2);
        const auto dynamicBody = Body(3);
        Register(*authority, dynamicBody, PhysicsMotionType::Dynamic, 3);
        Register(*authority, kinematicBody, PhysicsMotionType::Kinematic, 2);
        Register(*authority, staticBody, PhysicsMotionType::Static, 1);
        REQUIRE(authority->Activate().HasValue());

        const PhysicsDirectTransformWrite direct{.sceneGeneration = 9, .body = dynamicBody, .pose = Pose(99)};
        RequireCode(authority->WriteHostTransform(direct), PhysicsErrors::TransformAuthorityViolation);
        RequireCode(authority->WriteHostTransform({.sceneGeneration = 9, .body = kinematicBody, .pose = Pose(99)}),
                    PhysicsErrors::TransformAuthorityViolation);
        RequireCode(authority->WriteHostTransform({.sceneGeneration = 9, .body = staticBody, .pose = Pose(99)}),
                    PhysicsErrors::TransformAuthorityViolation);
        REQUIRE(authority->BodyTransform(dynamicBody).Value().runtimePose == Pose(3));

        RequireCode(authority->QueueTransformCommand(KinematicCommand(dynamicBody, 1, 10)), PhysicsErrors::TransformAuthorityViolation);
        RequireCode(authority->QueueTransformCommand(StaticCommand(kinematicBody, 1, 10, PhysicsStaticTransformUpdatePolicy::Rebuild)),
                    PhysicsErrors::TransformAuthorityViolation);
        RequireCode(authority->QueueTransformCommand(DynamicCommand(staticBody, 1, 10, PhysicsDynamicTransformOperation::Teleport,
                                                                    PhysicsTeleportVelocityPolicy::Reset)),
                    PhysicsErrors::TransformAuthorityViolation);
    }

    TEST_CASE("Kinematic targets are consumed once at their fixed tick and ignore render cadence", "[physics][transform][kinematic]") {
        auto authority = Prepared();
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Kinematic);
        REQUIRE(authority->Activate().HasValue());

        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 3, 30)).Value().status ==
                PhysicsTransformCommandAdmissionStatus::Deferred);
        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 1, 10)).Value().status ==
                PhysicsTransformCommandAdmissionStatus::Deferred);
        REQUIRE(authority->PendingCommandCount() == 2);
        REQUIRE(authority->ApplyPreStep(1).Value().appliedCommands == 1);
        REQUIRE(authority->BodyTransform(body).Value().runtimePose == Pose(10));
        REQUIRE(authority->ApplyPreStep(2).Value().appliedCommands == 0);
        REQUIRE(authority->BodyTransform(body).Value().runtimePose == Pose(10));

        const auto applied = authority->ApplyPreStep(3).Value();
        REQUIRE(applied.kinematicTargets == 1);
        REQUIRE(applied.appliedCommands == 1);
        REQUIRE(authority->PendingCommandCount() == 0);
        const auto state = authority->BodyTransform(body).Value();
        REQUIRE(state.runtimePose == Pose(30));
        REQUIRE(state.lastAppliedTick == 3);
        RequireCode(authority->ApplyPreStep(3), PhysicsErrors::CommandOrderInvalid);

        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 4, 40)).HasValue());
        RequireCode(authority->QueueTransformCommand(KinematicCommand(body, 4, 41, 2)), PhysicsErrors::CommandOrderInvalid);
    }

    TEST_CASE("Static updates preserve authored/runtime separation and report broadphase policy", "[physics][transform][static]") {
        auto authority = Prepared();
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Static, 1);
        REQUIRE(authority->Activate().HasValue());

        REQUIRE(
            authority->QueueTransformCommand(StaticCommand(body, 1, 5, PhysicsStaticTransformUpdatePolicy::UpdateBroadphase)).HasValue());
        const auto update = authority->ApplyPreStep(1).Value();
        REQUIRE(update.staticBroadphaseUpdates == 1);
        REQUIRE(update.staticRebuilds == 0);
        auto state = authority->BodyTransform(body).Value();
        REQUIRE(state.authoredPose == Pose(5));
        REQUIRE(state.runtimePose == Pose(5));

        REQUIRE(authority->QueueTransformCommand(StaticCommand(body, 2, 8, PhysicsStaticTransformUpdatePolicy::Rebuild)).HasValue());
        const auto rebuild = authority->ApplyPreStep(2).Value();
        REQUIRE(rebuild.staticBroadphaseUpdates == 0);
        REQUIRE(rebuild.staticRebuilds == 1);
        state = authority->BodyTransform(body).Value();
        REQUIRE(state.authoredPose == Pose(8));
        REQUIRE(state.runtimePose == Pose(8));
    }

    TEST_CASE("Dynamic snapshots are completed-tick evidence and never write back authored pose", "[physics][transform][dynamic]") {
        auto authority = Prepared();
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Dynamic, 1);
        REQUIRE(authority->Activate().HasValue());
        REQUIRE(authority
                    ->QueueTransformCommand(
                        DynamicCommand(body, 1, 4, PhysicsDynamicTransformOperation::Teleport, PhysicsTeleportVelocityPolicy::Preserve))
                    .HasValue());
        REQUIRE(authority->ApplyPreStep(1).Value().dynamicControls == 1);
        RequireCode(authority->DynamicSnapshot(body), PhysicsErrors::QuerySnapshotStale);

        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 1, 7, {4, 0, 0})).HasValue());
        auto state = authority->BodyTransform(body).Value();
        REQUIRE(state.authoredPose == Pose(1));
        REQUIRE(state.runtimePose == Pose(7));
        REQUIRE(state.lastPublishedTick == 1);
        REQUIRE(state.hasPublishedSnapshot);
        REQUIRE(authority->DynamicSnapshot(body).Value().linearVelocity == Math::Vec3{4, 0, 0});
        RequireCode(authority->PublishDynamicSnapshot(Snapshot(body, 1, 8)), PhysicsErrors::CommandOrderInvalid);

        REQUIRE(authority
                    ->QueueTransformCommand(
                        DynamicCommand(body, 2, 9, PhysicsDynamicTransformOperation::Reset, PhysicsTeleportVelocityPolicy::Reset))
                    .HasValue());
        REQUIRE(authority->ApplyPreStep(2).Value().dynamicControls == 1);
        state = authority->BodyTransform(body).Value();
        REQUIRE_FALSE(state.hasPublishedSnapshot);
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 2, 9, {0, 0, 0})).HasValue());
        REQUIRE(authority->BodyTransform(body).Value().authoredPose == Pose(1));

        auto invalid = Snapshot(body, 2, 9);
        invalid.state.pose.translation.x = std::numeric_limits<float>::quiet_NaN();
        RequireCode(ValidatePhysicsDynamicTransformSnapshot(invalid, World(), 9, 2), PhysicsErrors::DescriptorInvalid);
    }

    TEST_CASE("Transform authority enforces bounded command admission and lifecycle", "[physics][transform][lifecycle]") {
        auto authority = Prepared(1, 1);
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Kinematic);
        REQUIRE(authority->Activate().HasValue());
        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 1, 1)).Value().status ==
                PhysicsTransformCommandAdmissionStatus::Deferred);
        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 2, 2)).Value().status ==
                PhysicsTransformCommandAdmissionStatus::RejectedFull);
        REQUIRE(authority->ApplyPreStep(1).HasValue());
        REQUIRE(authority->QueueTransformCommand(KinematicCommand(body, 2, 2)).Value().status ==
                PhysicsTransformCommandAdmissionStatus::Deferred);

        auto foreignError = false;
        std::thread foreign([&] {
            const auto result = authority->ApplyPreStep(2);
            foreignError = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(foreignError);
        REQUIRE(authority->ApplyPreStep(2).HasValue());

        REQUIRE(authority->UnregisterBody(body).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        authority->Shutdown();
        REQUIRE(authority->State() == PhysicsTransformAuthorityState::Destroyed);
        RequireCode(authority->BodyTransform(body), PhysicsErrors::InvalidState);
    }
}  // namespace Horo::Physics
