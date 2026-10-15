#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Horo::Character {
    namespace {
        /** @brief Rejects unsupported providers and mismatched frame evidence before any query callback executes. */
        Result<void> ValidateFrame(const CharacterPhysicsQueryContext &context) {
            if (context.frameCapability != CharacterQueryFrameCapability::CapturedOnly &&
                context.frameCapability != CharacterQueryFrameCapability::PredictedBodyPose)
                return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported));
            if (!context.bodyPoseFrame)
                return Result<void>::Success();
            if (context.frameCapability != CharacterQueryFrameCapability::PredictedBodyPose)
                return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported));
            const auto &frame = *context.bodyPoseFrame;
            if (frame.participation != Physics::PhysicsQueryFrameParticipation::PredictedPose &&
                frame.participation != Physics::PhysicsQueryFrameParticipation::CoMovingCarry)
                return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported));
            if (frame.consumingTick != context.tick || frame.sourcePublicationRevision != context.physicsSnapshotRevision)
                return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
            if (const auto body = Physics::ValidatePhysicsHandleOwner(frame.body, context.physicsWorld); body.HasError())
                return body;
            if (const auto shape = Physics::ValidatePhysicsHandleOwner(frame.shape, context.physicsWorld); shape.HasError())
                return shape;
            for (const auto &pose : {frame.sourcePose, frame.predictedPose}) {
                if (const auto valid = Physics::ValidatePhysicsPose(pose); valid.HasError())
                    return valid;
                if (std::abs(pose.translation.x) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
                    std::abs(pose.translation.y) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
                    std::abs(pose.translation.z) > Physics::MaximumPhysicsLocalHalfExtentMeters)
                    return Result<void>::Failure(MakeError(CharacterErrors::RequestInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateCharacterPhysicsQueryContext */
    Result<void> ValidateCharacterPhysicsQueryContext(const CharacterPhysicsQueryContext &context,
                                                      const CharacterPhysicsQueryExpectations &expected) {
        if (const std::array expectedValid{expected.sceneGeneration != 0, expected.characterWorld.IsValid(),
                                           expected.physicsWorld.IsValid(), expected.collisionFilterGeneration != 0,
                                           expected.originGeneration != 0, expected.physicsSnapshotRevision != 0};
            !std::ranges::all_of(expectedValid, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::WorldInvalid));
        if (const std::array ownerMatches{context.sceneGeneration == expected.sceneGeneration,
                                          context.characterWorld == expected.characterWorld, context.physicsWorld == expected.physicsWorld};
            !std::ranges::all_of(ownerMatches, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::HandleWorldMismatch));
        if (const std::array snapshotMatches{context.collisionFilterGeneration == expected.collisionFilterGeneration,
                                             context.originGeneration == expected.originGeneration, context.tick == expected.tick,
                                             context.physicsSnapshotRevision == expected.physicsSnapshotRevision};
            !std::ranges::all_of(snapshotMatches, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        if (context.overlap == nullptr && context.sweep == nullptr)
            return Result<void>::Failure(
                MakeError(CharacterErrors::OperationUnsupported, "Character movement requires a Physics overlap or sweep probe."));
        return ValidateFrame(context);
    }

}  // namespace Horo::Character
