#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @copydoc ValidateCanonicalContinuousCollision */
    Result<void> ValidateCanonicalContinuousCollision(const CanonicalWorldHandle world, const PhysicsContinuousCollisionPolicy &policy) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto valid = ValidatePhysicsContinuousCollisionPolicy(policy); valid.HasError())
            return valid;
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        if (canonical.continuousCollision.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (const auto &body : canonical.scene.bodies) {
            JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
            if (!lock.Succeeded())
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto quality = ResolveCanonicalMotionQuality(policy, body.policy, *lock.GetBody().GetShape(), lock.GetBody().IsSensor());
            if (quality.HasError())
                return Result<void>::Failure(quality.ErrorValue());
            if (quality.Value() == JPH::EMotionQuality::LinearCast && lock.GetBody().GetObjectLayer() == 0)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapabilityUnavailable, "LinearCast cannot be enabled on an unbound simulation body."));
        }
        return Result<void>::Success();
    }

    /** @copydoc ApplyCanonicalContinuousCollision */
    Result<void> ApplyCanonicalContinuousCollision(const CanonicalWorldHandle world, const PhysicsContinuousCollisionPolicy &policy) {
        if (const auto valid = ValidateCanonicalContinuousCollision(world, policy); valid.HasError())
            return valid;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.continuousCollision.policy == policy)
            return Result<void>::Success();
        auto settings = canonical.native.system->GetPhysicsSettings();
        settings.mLinearCastThreshold = policy.thresholdFraction;
        settings.mLinearCastMaxPenetration = policy.penetrationFraction;
        canonical.native.system->SetPhysicsSettings(settings);
        auto &interface = canonical.native.system->GetBodyInterface();
        // Pre-step validation retained every resident ID and shape; no provider or native work runs between these phases.
        for (const auto &body : canonical.scene.bodies) {
            JPH::EMotionQuality quality;
            bool affected{};
            {
                JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
                const auto &native = lock.GetBody();
                quality = ResolveCanonicalMotionQuality(policy, body.policy, *native.GetShape(), native.IsSensor()).Value();
                affected = !native.IsStatic() && (native.GetMotionProperties()->GetMotionQuality() == JPH::EMotionQuality::LinearCast ||
                                                  quality == JPH::EMotionQuality::LinearCast);
            }
            if (body.policy.motion != PhysicsMotionType::Static)
                interface.SetMotionQuality(body.nativeBody, quality);
            if (affected) {
                interface.InvalidateContactCache(body.nativeBody);
                interface.ActivateBody(body.nativeBody);
            }
        }
        canonical.continuousCollision.policy = policy;
        ++canonical.continuousCollision.revision;
        return Result<void>::Success();
    }

    /** @copydoc ReadCanonicalContinuousCollision */
    Result<PhysicsContinuousCollisionObservation> ReadCanonicalContinuousCollision(const CanonicalWorldHandle world) {
        if (world.value == nullptr)
            return Result<PhysicsContinuousCollisionObservation>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        return Result<PhysicsContinuousCollisionObservation>::Success(
            static_cast<const CanonicalWorld *>(world.value)->continuousCollision);
    }
}  // namespace Horo::Physics::Detail
