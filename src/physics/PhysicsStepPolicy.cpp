#include "Horo/Physics/PhysicsStepPolicy.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>

namespace Horo::Physics {
    namespace {
        /** @brief Checks the scalar representation of a positive SI sleep threshold. */
        bool PositiveFiniteThreshold(const float value) noexcept {
            return std::isfinite(value) && value > 0;
        }
    }  // namespace

    /** @copydoc ValidatePhysicsContinuousCollisionPolicy */
    Result<void> ValidatePhysicsContinuousCollisionPolicy(const PhysicsContinuousCollisionPolicy &policy) {
        if (policy.defaultMode > PhysicsDefaultMotionQuality::LinearCast)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown continuous collision mode."));
        if (!std::isfinite(policy.thresholdFraction) || !std::isfinite(policy.penetrationFraction) || policy.thresholdFraction <= 0.0F ||
            policy.thresholdFraction > 1.0F || policy.penetrationFraction <= 0.0F || policy.penetrationFraction > 1.0F)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "CCD fractions must be finite in (0,1]."));
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsStepPolicy */
    Result<void> ValidatePhysicsStepPolicy(const PhysicsStepPolicy &policy) {
        if (policy.substepsPerTick == 0 || policy.velocityIterations == 0 || policy.positionIterations == 0 ||
            !PositiveFiniteThreshold(policy.sleepPointSpeedMetersPerSecond) || !PositiveFiniteThreshold(policy.sleepDelaySeconds))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Step counts and finite sleep thresholds must be positive."));
        if (policy.defaultMotionQuality != PhysicsDefaultMotionQuality::Discrete &&
            policy.defaultMotionQuality != PhysicsDefaultMotionQuality::LinearCast)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown default motion-quality policy."));
        if (const auto ccd = ValidatePhysicsContinuousCollisionPolicy(
                {policy.defaultMotionQuality, policy.linearCastThresholdFraction, policy.linearCastPenetrationFraction});
            ccd.HasError())
            return ccd;
        PhysicsStepPolicy qualified = policy;
        qualified.defaultMotionQuality = PhysicsDefaultMotionQuality::Discrete;
        qualified.linearCastThresholdFraction = 0.75F;
        qualified.linearCastPenetrationFraction = 0.25F;
        if (qualified != PhysicsStepPolicy{})
            return Result<void>::Failure(
                MakeError(PhysicsErrors::ProfileUnsupported,
                          "CanonicalV1 requires 1 substep, 10/2 iterations, 0.03 m/s for 0.5 s sleep and bounded CCD policy."));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
