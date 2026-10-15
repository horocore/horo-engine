#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <algorithm>
#include <type_traits>

namespace Horo::Physics::Detail {
    /** @copydoc ResolveCanonicalQueryFrame */
    Result<JPH::BodyID> ResolveCanonicalQueryFrame(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor) {
        if (!descriptor.bodyPoseFrame)
            return Result<JPH::BodyID>::Success(JPH::BodyID{});
        const auto &frame = *descriptor.bodyPoseFrame;
        const auto found = std::ranges::find(access.sceneBodies, frame.body, &CanonicalSceneBodyRecord::handle);
        if (found == access.sceneBodies.end() || found->policy.shape != frame.shape)
            return Result<JPH::BodyID>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (!AdmitsCanonicalSceneQueryBody(access, descriptor, found->nativeBody))
            return Result<JPH::BodyID>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        JPH::BodyLockRead lock(access.system.GetBodyLockInterface(), found->nativeBody);
        if (!lock.Succeeded())
            return Result<JPH::BodyID>::Failure(MakeError(PhysicsErrors::HandleStale));
        const auto &body = lock.GetBody();
        const auto position = body.GetPosition();
        const auto rotation = body.GetRotation();
        const PhysicsPose actual{{position.GetX(), position.GetY(), position.GetZ()},
                                 {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()}};
        if (actual != frame.sourcePose)
            return Result<JPH::BodyID>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<JPH::BodyID>::Success(found->nativeBody);
    }

    /** @copydoc CollectPredictedSupport */
    Result<void> CollectPredictedSupport(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                         CanonicalQueryCollectors &collectors, const JPH::BodyID body) {
        if (!descriptor.bodyPoseFrame || descriptor.bodyPoseFrame->participation == PhysicsQueryFrameParticipation::CoMovingCarry)
            return Result<void>::Success();
        const auto overlapStart = collectors.overlap.count;
        const auto sweepStart = collectors.sweep.count;
        const auto mapped = MapCanonicalFrameQuery(descriptor);
        if (const auto valid = ValidatePhysicsQueryDescriptor(mapped, descriptor.world, descriptor.sceneGeneration); valid.HasError())
            return valid;
        if (const auto collected = CollectCanonicalFramedGeometry(access, mapped, descriptor, collectors, body); collected.HasError())
            return collected;
        for (auto index = overlapStart; index < collectors.overlap.count; ++index)
            RemapCanonicalFrameHit(*descriptor.bodyPoseFrame, collectors.overlap.values[index]);
        for (auto index = sweepStart; index < collectors.sweep.count; ++index)
            RemapCanonicalFrameHit(*descriptor.bodyPoseFrame, collectors.sweep.values[index]);
        return Result<void>::Success();
    }

    /** @copydoc MapCanonicalFrameQuery */
    PhysicsQueryDescriptor MapCanonicalFrameQuery(const PhysicsQueryDescriptor &descriptor) {
        auto mapped = descriptor;
        const auto &frame = *descriptor.bodyPoseFrame;
        const auto rotation = frame.sourcePose.rotation * frame.predictedPose.rotation.Inverse();
        std::visit([&]<typename Query>(Query &query) {
            if constexpr (std::is_same_v<Query, PhysicsCapsuleOverlapQuery> || std::is_same_v<Query, PhysicsCapsuleSweepQuery>) {
                query.position = frame.sourcePose.translation + rotation.Rotate(query.position - frame.predictedPose.translation);
                query.up = rotation.Rotate(query.up);
                if constexpr (std::is_same_v<Query, PhysicsCapsuleSweepQuery>)
                    query.direction = rotation.Rotate(query.direction);
            }
        }, mapped.geometry);
        return mapped;
    }

    /** @copydoc RemapCanonicalFrameHit */
    void RemapCanonicalFrameHit(const PhysicsQueryBodyPoseFrame &frame, JPH::CollideShapeResult &hit) noexcept {
        const auto rotation = frame.predictedPose.rotation * frame.sourcePose.rotation.Inverse();
        const auto point = [&](const JPH::Vec3 value) {
            const auto mapped = frame.predictedPose.translation +
                                rotation.Rotate(Math::Vec3{value.GetX(), value.GetY(), value.GetZ()} - frame.sourcePose.translation);
            return JPH::Vec3{mapped.x, mapped.y, mapped.z};
        };
        hit.mContactPointOn1 = point(hit.mContactPointOn1);
        hit.mContactPointOn2 = point(hit.mContactPointOn2);
        const auto axis = rotation.Rotate({hit.mPenetrationAxis.GetX(), hit.mPenetrationAxis.GetY(), hit.mPenetrationAxis.GetZ()});
        hit.mPenetrationAxis = {axis.x, axis.y, axis.z};
    }
}  // namespace Horo::Physics::Detail
