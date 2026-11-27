#include "Horo/Physics/PhysicsPose.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>

namespace Horo::Physics {
    /** @copydoc ValidatePhysicsPose */
    Result<void> ValidatePhysicsPose(const PhysicsPose &pose) {
        if (!Math::IsFinite(pose.translation) || !Math::IsFinite(pose.rotation))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics pose coordinates and rotation must be finite."));
        if (const double squaredNorm =
                static_cast<double>(pose.rotation.x) * pose.rotation.x + static_cast<double>(pose.rotation.y) * pose.rotation.y +
                static_cast<double>(pose.rotation.z) * pose.rotation.z + static_cast<double>(pose.rotation.w) * pose.rotation.w;
            std::abs(squaredNorm - 1.0) > PhysicsRotationSquaredNormTolerance)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics rotation must have unit squared norm within 1e-6."));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
