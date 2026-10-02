#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTransformAuthority.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>
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
        invalid.state.activity = PhysicsBodyActivity::Static;
        RequireCode(ValidatePhysicsDynamicTransformSnapshot(invalid, World(), 9, 2), PhysicsErrors::DescriptorInvalid);
        invalid.state.activity = PhysicsBodyActivity::Sleeping;
        REQUIRE(ValidatePhysicsDynamicTransformSnapshot(invalid, World(), 9, 2).HasValue());
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

    TEST_CASE("Completed Physics pose pairs render equivalent motion at independent presentation rates",
              "[physics][transform][interpolation]") {
        auto authority = Prepared();
        const auto dynamic = Body(1);
        const auto kinematic = Body(2);
        const auto staticBody = Body(3);
        Register(*authority, dynamic, PhysicsMotionType::Dynamic);
        Register(*authority, kinematic, PhysicsMotionType::Kinematic);
        Register(*authority, staticBody, PhysicsMotionType::Static);
        REQUIRE(authority->Activate().HasValue());
        RequireCode(authority->InterpolationEndpoints(dynamic), PhysicsErrors::QuerySnapshotStale);

        REQUIRE(authority->ApplyPreStep(1).HasValue());
        RequireCode(authority->CommitInterpolationTick(1), PhysicsErrors::QuerySnapshotStale);
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(dynamic, 1, 0)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(1).HasValue());
        const auto first = authority->InterpolationEndpoints(dynamic).Value();
        REQUIRE_FALSE(first.hasPreviousTick);
        REQUIRE(EvaluatePhysicsInterpolation(first, 0.75F).Value() == Pose(0));

        REQUIRE(authority->QueueTransformCommand(KinematicCommand(kinematic, 2, 20)).HasValue());
        REQUIRE(authority->ApplyPreStep(2).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(dynamic, 2, 10)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(2).HasValue());
        const auto dynamicPair = authority->InterpolationEndpoints(dynamic).Value();
        const auto kinematicPair = authority->InterpolationEndpoints(kinematic).Value();
        REQUIRE(dynamicPair.hasPreviousTick);
        REQUIRE(dynamicPair.previousTick == 1);
        REQUIRE(dynamicPair.currentTick == 2);
        REQUIRE(kinematicPair.hasPreviousTick);
        for (const float alpha : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
            REQUIRE(EvaluatePhysicsInterpolation(dynamicPair, alpha).Value().translation.x == 10.0F * alpha);
            REQUIRE(EvaluatePhysicsInterpolation(kinematicPair, alpha).Value().translation.x == 20.0F * alpha);
        }
        REQUIRE(authority->BodyTransform(dynamic).Value().authoredPose == Pose(0));
        REQUIRE(authority->DynamicSnapshot(dynamic).Value().pose == Pose(10));

        REQUIRE(authority->QueueTransformCommand(StaticCommand(staticBody, 3, 5, PhysicsStaticTransformUpdatePolicy::UpdateBroadphase))
                    .HasValue());
        REQUIRE(authority->ApplyPreStep(3).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(dynamic, 3, 20)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(3).HasValue());
        REQUIRE(EvaluatePhysicsInterpolation(dynamicPair, 0.5F).Value() == Pose(5));
        REQUIRE(EvaluatePhysicsInterpolation(authority->InterpolationEndpoints(dynamic).Value(), 0.5F).Value() == Pose(15));
        const auto staticPair = authority->InterpolationEndpoints(staticBody).Value();
        REQUIRE_FALSE(staticPair.hasPreviousTick);
        REQUIRE(EvaluatePhysicsInterpolation(staticPair, 0.5F).Value() == Pose(5));
    }

    TEST_CASE("Physics presentation uses spherical rotation interpolation", "[physics][transform][interpolation]") {
        auto authority = Prepared();
        const auto dynamic = Body();
        Register(*authority, dynamic, PhysicsMotionType::Dynamic);
        REQUIRE(authority->Activate().HasValue());
        REQUIRE(authority->ApplyPreStep(1).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(dynamic, 1, 20)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(1).HasValue());

        REQUIRE(authority->ApplyPreStep(2).HasValue());
        auto rotated = Snapshot(dynamic, 2, 30);
        rotated.state.pose.rotation = {0, 0, 1, 0};
        REQUIRE(authority->PublishDynamicSnapshot(rotated).HasValue());
        REQUIRE(authority->CommitInterpolationTick(2).HasValue());
        const auto midpoint = EvaluatePhysicsInterpolation(authority->InterpolationEndpoints(dynamic).Value(), 0.5F).Value();
        REQUIRE(midpoint.translation.x == 25.0F);
        REQUIRE(std::abs(midpoint.rotation.z - 0.70710678F) < 1.0e-5F);
        REQUIRE(std::abs(midpoint.rotation.w - 0.70710678F) < 1.0e-5F);
    }

    TEST_CASE("Teleport restore reload and origin shift never interpolate across a discontinuity",
              "[physics][transform][interpolation][lifecycle]") {
        auto authority = Prepared();
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Dynamic, 1);
        REQUIRE(authority->Activate().HasValue());
        REQUIRE(authority->ApplyPreStep(1).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 1, 0)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(1).HasValue());
        REQUIRE(authority->ApplyPreStep(2).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 2, 10)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(2).HasValue());

        REQUIRE(authority->RebaseInterpolationHistory({100, 0, 0}, 2).HasValue());
        auto pair = authority->InterpolationEndpoints(body).Value();
        REQUIRE(pair.originGeneration == 2);
        REQUIRE(pair.previousPose == Pose(-100));
        REQUIRE(pair.currentPose == Pose(-90));
        REQUIRE(EvaluatePhysicsInterpolation(pair, 0.5F).Value() == Pose(-95));

        REQUIRE(authority
                    ->QueueTransformCommand(
                        DynamicCommand(body, 3, 100, PhysicsDynamicTransformOperation::Teleport, PhysicsTeleportVelocityPolicy::Reset))
                    .HasValue());
        REQUIRE(authority->ApplyPreStep(3).HasValue());
        REQUIRE(authority->InterpolationEndpoints(body).Value().currentTick == 2);
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 3, 100)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(3).HasValue());
        pair = authority->InterpolationEndpoints(body).Value();
        REQUIRE_FALSE(pair.hasPreviousTick);
        REQUIRE(EvaluatePhysicsInterpolation(pair, 0.5F).Value() == Pose(100));

        REQUIRE(authority->ApplyPreStep(4).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 4, 110)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(4).HasValue());
        REQUIRE(EvaluatePhysicsInterpolation(authority->InterpolationEndpoints(body).Value(), 0.5F).Value() == Pose(105));
        REQUIRE(authority->ResetInterpolationHistory(PhysicsInterpolationResetReason::Restore).HasValue());
        RequireCode(authority->InterpolationEndpoints(body), PhysicsErrors::QuerySnapshotStale);

        REQUIRE(authority->ApplyPreStep(5).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 5, 200)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(5).HasValue());
        REQUIRE_FALSE(authority->InterpolationEndpoints(body).Value().hasPreviousTick);
        REQUIRE(authority->ResetInterpolationHistory(PhysicsInterpolationResetReason::Reload).HasValue());
        RequireCode(authority->InterpolationEndpoints(body), PhysicsErrors::QuerySnapshotStale);
        authority->Shutdown();
        RequireCode(authority->InterpolationEndpoints(body), PhysicsErrors::InvalidState);
    }

    TEST_CASE("Interpolation rejects malformed cadence and preserves the last complete pose pair on failure",
              "[physics][transform][interpolation][failure]") {
        auto authority = Prepared();
        const auto body = Body();
        Register(*authority, body, PhysicsMotionType::Dynamic);
        REQUIRE(authority->Activate().HasValue());
        REQUIRE(authority->ApplyPreStep(1).HasValue());
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 1, 0)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(1).HasValue());
        const auto copied = authority->InterpolationEndpoints(body).Value();
        RequireCode(EvaluatePhysicsInterpolation(copied, -0.1F), PhysicsErrors::DescriptorInvalid);
        RequireCode(EvaluatePhysicsInterpolation(copied, 1.1F), PhysicsErrors::DescriptorInvalid);
        RequireCode(EvaluatePhysicsInterpolation(copied, std::numeric_limits<float>::quiet_NaN()), PhysicsErrors::DescriptorInvalid);
        auto malformed = copied;
        malformed.hasPreviousTick = true;
        malformed.previousTick = 1;
        RequireCode(EvaluatePhysicsInterpolation(malformed, 0.5F), PhysicsErrors::DescriptorInvalid);
        RequireCode(authority->CommitInterpolationTick(1), PhysicsErrors::CommandOrderInvalid);

        REQUIRE(authority->ApplyPreStep(2).HasValue());
        RequireCode(authority->CommitInterpolationTick(2), PhysicsErrors::QuerySnapshotStale);
        RequireCode(authority->ResetInterpolationHistory(PhysicsInterpolationResetReason::Restore), PhysicsErrors::InvalidState);
        REQUIRE(authority->InterpolationEndpoints(body).Value().currentTick == 1);
        REQUIRE(authority->PublishDynamicSnapshot(Snapshot(body, 2, 10)).HasValue());
        REQUIRE(authority->CommitInterpolationTick(2).HasValue());
        RequireCode(authority->PublishDynamicSnapshot(Snapshot(body, 2, 11)), PhysicsErrors::CommandOrderInvalid);
        RequireCode(authority->RebaseInterpolationHistory({std::numeric_limits<float>::infinity(), 0, 0}, 2),
                    PhysicsErrors::DescriptorInvalid);
        REQUIRE(authority->InterpolationEndpoints(body).Value().originGeneration == 1);
        RequireCode(authority->RebaseInterpolationHistory({1, 0, 0}, 1), PhysicsErrors::DescriptorInvalid);
        RequireCode(authority->ResetInterpolationHistory(static_cast<PhysicsInterpolationResetReason>(255)),
                    PhysicsErrors::OperationUnsupported);
        RequireCode(authority->InterpolationEndpoints(Body(9)), PhysicsErrors::HandleStale);
        REQUIRE(authority
                    ->QueueTransformCommand(
                        DynamicCommand(body, 3, 20, PhysicsDynamicTransformOperation::Teleport, PhysicsTeleportVelocityPolicy::Reset))
                    .HasValue());
        RequireCode(authority->RebaseInterpolationHistory({1, 0, 0}, 2), PhysicsErrors::InvalidState);

        bool rejectedForeignRead = false;
        std::thread foreign([&] {
            const auto result = authority->InterpolationEndpoints(body);
            rejectedForeignRead =
                result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(rejectedForeignRead);
    }
}  // namespace Horo::Physics
