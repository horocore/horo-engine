#include "Horo/Physics/PhysicsConstraintDescriptor.h"

#include "Horo/Physics/PhysicsCapabilities.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Physics {
    namespace {
        /** @brief Validates a body-local anchor without resolving or retaining the body. */
        Result<void> ValidateBodyAnchor(const PhysicsBodyAnchor &anchor, const PhysicsWorldId world) {
            if (const auto owner = ValidatePhysicsHandleOwner(anchor.body, world); owner.HasError())
                return owner;
            return ValidatePhysicsPose(anchor.localFrame);
        }

        /** @brief Validates the explicit second endpoint and rejects self-constraints. */
        Result<void> ValidateSecondAnchor(const PhysicsConstraintDescriptor &descriptor, const PhysicsWorldId world) {
            const auto *body = std::get_if<PhysicsBodyAnchor>(&descriptor.second);
            if (body == nullptr)
                return ValidatePhysicsPose(std::get<PhysicsWorldAnchor>(descriptor.second).frame);
            if (const auto valid = ValidateBodyAnchor(*body, world); valid.HasError())
                return valid;
            if (body->body == descriptor.first.body)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Constraint endpoints must name distinct bodies."));
            return Result<void>::Success();
        }

        /** @brief Checks finite non-negative ordered distance limits without clamping. */
        Result<void> ValidateDistance(const PhysicsDistanceConstraint &distance) {
            if (const std::array bounds{distance.minimumMeters, distance.maximumMeters}; !std::ranges::all_of(bounds,
                                                                                                              [](const float value) {
                return std::isfinite(value);
            }) || distance.minimumMeters < 0 || distance.maximumMeters < distance.minimumMeters)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Constraint distances must be finite with 0 <= minimum <= maximum."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePhysicsConstraintDescriptor */
    Result<void> ValidatePhysicsConstraintDescriptor(const PhysicsConstraintDescriptor &descriptor, const PhysicsWorldId expectedWorld) {
        if (const auto first = ValidateBodyAnchor(descriptor.first, expectedWorld); first.HasError())
            return first;
        if (const auto second = ValidateSecondAnchor(descriptor, expectedWorld); second.HasError())
            return second;
        if (descriptor.collisionPolicy != PhysicsJointCollisionPolicy::DisableBetweenBodies &&
            descriptor.collisionPolicy != PhysicsJointCollisionPolicy::AllowBetweenBodies)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Unknown joint collision policy."));
        if (const auto *distance = std::get_if<PhysicsDistanceConstraint>(&descriptor.parameters))
            return ValidateDistance(*distance);
        return Result<void>::Success();
    }

    /** @copydoc AdmitPhysicsConstraintDescriptor */
    Result<void> AdmitPhysicsConstraintDescriptor(const PhysicsConstraintDescriptor &descriptor, const PhysicsWorldId expectedWorld,
                                                  const PhysicsCapabilities &capabilities, const std::uint64_t expectedRevision) {
        if (const auto validation = ValidatePhysicsConstraintDescriptor(descriptor, expectedWorld); validation.HasError())
            return validation;
        return RequirePhysicsCapability(capabilities, PhysicsCapability::Constraints, expectedRevision);
    }
}  // namespace Horo::Physics
