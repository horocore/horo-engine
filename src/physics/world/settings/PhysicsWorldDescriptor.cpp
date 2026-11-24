#include "Horo/Physics/PhysicsWorldDescriptor.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>
#include <limits>

namespace Horo::Physics {
    namespace {
        /** @brief Compare independent admission limits; never add potentially overflowing counts. */
        bool ValidCapacity(const PhysicsWorldCapacity &capacity) noexcept {
            return capacity.maximumBodies <= MaximumPhysicsBodies && capacity.maximumColliderSlots <= MaximumPhysicsColliderSlots &&
                   capacity.maximumConstraints <= MaximumPhysicsConstraints && capacity.maximumPlanBytes != 0 &&
                   capacity.maximumPlanBytes <= MaximumPhysicsPlanBytes;
        }

        /** @brief Double intermediates safely cover every finite float gravity vector before the magnitude check. */
        bool ValidGravity(const Math::Vec3 gravity) noexcept {
            const double x = gravity.x;
            const double y = gravity.y;
            const double z = gravity.z;
            return Math::IsFinite(gravity) && x * x + y * y + z * z <= MaximumPhysicsGravityMagnitude * MaximumPhysicsGravityMagnitude;
        }

        /** @brief Reject underflow/subnormal or overflowing native delta before any double-to-float conversion. */
        bool ValidFixedDelta(const double seconds) noexcept {
            return std::isfinite(seconds) && seconds >= std::numeric_limits<float>::min() && seconds <= std::numeric_limits<float>::max();
        }
    }  // namespace

    /** @copydoc ValidatePhysicsWorldDescriptor */
    Result<void> ValidatePhysicsWorldDescriptor(const PhysicsWorldDescriptor &descriptor) {
        if (descriptor.contractVersion != 1)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Unsupported physics world descriptor version."));
        if (descriptor.profile != PhysicsToleranceProfileId::CanonicalV1)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        if (!ValidGravity(descriptor.gravity) || !ValidFixedDelta(descriptor.fixedDeltaSeconds))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid,
                          "Provide finite gravity within 20 m/s² and a positive fixed delta in the normal fp32 range."));
        if (!ValidCapacity(descriptor.capacity))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
