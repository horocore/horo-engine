#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTransformAuthority.h"

#include <cmath>

namespace Horo::Physics {
    /** @copydoc EvaluatePhysicsInterpolation */
    Result<PhysicsPose> EvaluatePhysicsInterpolation(const PhysicsInterpolationEndpoints &endpoints, const float alpha) {
        if (endpoints.sceneGeneration == 0 || endpoints.originGeneration == 0 || endpoints.currentTick == 0 || !std::isfinite(alpha) ||
            alpha < 0.0F || alpha > 1.0F ||
            (endpoints.hasPreviousTick && (endpoints.previousTick == 0 || endpoints.previousTick >= endpoints.currentTick ||
                                           endpoints.currentTick - endpoints.previousTick != 1)))
            return Result<PhysicsPose>::Failure(MakeError(PhysicsErrors::DescriptorInvalid,
                                                          "Interpolation requires a completed consecutive tick pair and alpha in [0, 1]."));
        if (const auto handle = ValidatePhysicsHandleOwner(endpoints.body, endpoints.body.world); handle.HasError())
            return Result<PhysicsPose>::Failure(handle.ErrorValue());
        if (const auto valid = ValidatePhysicsPose(endpoints.currentPose); valid.HasError())
            return Result<PhysicsPose>::Failure(valid.ErrorValue());
        if (!endpoints.hasPreviousTick)
            return Result<PhysicsPose>::Success(endpoints.currentPose);
        if (const auto valid = ValidatePhysicsPose(endpoints.previousPose); valid.HasError())
            return Result<PhysicsPose>::Failure(valid.ErrorValue());
        return Result<PhysicsPose>::Success(
            {.translation = Math::Lerp(endpoints.previousPose.translation, endpoints.currentPose.translation, alpha),
             .rotation = Math::Slerp(endpoints.previousPose.rotation, endpoints.currentPose.rotation, alpha)});
    }

    /** @copydoc ResolvePhysicsTransformAuthority */
    Result<PhysicsTransformAuthority> ResolvePhysicsTransformAuthority(const PhysicsMotionType motion) {
        using enum PhysicsMotionType;
        using enum PhysicsTransformAuthority;
        switch (motion) {
            case Static:
                return Result<PhysicsTransformAuthority>::Success(StaticScene);
            case Kinematic:
                return Result<PhysicsTransformAuthority>::Success(KinematicTarget);
            case Dynamic:
                return Result<PhysicsTransformAuthority>::Success(DynamicSolver);
        }
        return Result<PhysicsTransformAuthority>::Failure(
            MakeError(PhysicsErrors::OperationUnsupported, "Unknown body motion mode has no transform authority."));
    }
}  // namespace Horo::Physics
