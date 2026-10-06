#include "Horo/Physics/PhysicsBodyDescriptor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Physics {
    namespace {
        /** @brief Validates mass or density magnitude without silently clamping or deriving inertia. */
        Result<void> ValidateMassMagnitude(const float value, const float minimum, const float maximum) {
            if (!std::isfinite(value) || value < minimum || value > maximum)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Dynamic mass or density is outside CanonicalV1 bounds."));
            return Result<void>::Success();
        }

        /** @brief Requires an explicit dynamic policy, and absence of dynamic mass for other modes. */
        Result<void> ValidateMassPolicy(const PhysicsMotionType motion, const PhysicsMassPolicy &policy) {
            const bool noMass = std::holds_alternative<PhysicsNoMass>(policy);
            if (motion != PhysicsMotionType::Dynamic) {
                if (!noMass)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Static and kinematic bodies must use PhysicsNoMass."));
                return Result<void>::Success();
            }
            if (noMass)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Dynamic bodies require explicit mass or density."));
            if (const auto *mass = std::get_if<PhysicsMass>(&policy))
                return ValidateMassMagnitude(mass->kilograms, MinimumPhysicsMassKilograms, MaximumPhysicsMassKilograms);
            return ValidateMassMagnitude(std::get<PhysicsDensity>(policy).kilogramsPerCubicMeter, MinimumPhysicsDensity,
                                         MaximumPhysicsDensity);
        }

        /** @brief Computes squared vector magnitude with double intermediates. */
        double SquaredMagnitude(const Math::Vec3 value) noexcept {
            return static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
        }

        /** @brief Validates bounded frame-rate-independent motion safety policy. */
        Result<void> ValidateMotionSafety(const PhysicsMotionSafety &safety) {
            if (const auto inRange =
                    [](const float value, const float maximum) {
                return std::isfinite(value) && value >= 0.0F && value <= maximum;
            };
                !inRange(safety.linearDampingPerSecond, MaximumPhysicsDampingPerSecond) ||
                !inRange(safety.angularDampingPerSecond, MaximumPhysicsDampingPerSecond) ||
                !inRange(safety.maximumLinearSpeed, static_cast<float>(MaximumPhysicsLinearSpeed)) ||
                !inRange(safety.maximumAngularSpeed, MaximumPhysicsAngularSpeed) ||
                !inRange(safety.maximumDepenetrationSpeed, MaximumPhysicsDepenetrationSpeed) ||
                (static_cast<unsigned int>(safety.lockedAxes) & ~static_cast<unsigned int>(PhysicsAxisLock::All)) != 0U)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Body motion safety policy is invalid."));
            return Result<void>::Success();
        }

        /** @brief Checks finite velocities and the descriptor's explicit speed ceilings. */
        Result<void> ValidateInitialVelocity(const PhysicsMotionType motion, const Math::Vec3 linearVelocity,
                                             const Math::Vec3 angularVelocity, const PhysicsMotionSafety &safety) {
            if (const auto policy = ValidateMotionSafety(safety); policy.HasError())
                return policy;
            if (const std::array velocities{linearVelocity, angularVelocity}; !std::ranges::all_of(velocities, [](const Math::Vec3 value) {
                return Math::IsFinite(value);
            }))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Initial body velocities must be finite."));
            if (SquaredMagnitude(linearVelocity) > static_cast<double>(safety.maximumLinearSpeed) * safety.maximumLinearSpeed)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Initial linear speed exceeds its admitted limit."));
            if (SquaredMagnitude(angularVelocity) > static_cast<double>(safety.maximumAngularSpeed) * safety.maximumAngularSpeed)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Initial angular speed exceeds its admitted limit."));
            if (motion == PhysicsMotionType::Static && (linearVelocity != Math::Vec3{} || angularVelocity != Math::Vec3{}))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Static bodies cannot have initial velocity."));
            return Result<void>::Success();
        }

        /** @brief Checks finite observed velocities without applying authored admission limits to solver output. */
        bool FiniteStateVelocity(const PhysicsBodyState &state) noexcept {
            return Math::IsFinite(state.linearVelocity) && Math::IsFinite(state.angularVelocity);
        }
    }  // namespace

    /** @copydoc ValidatePhysicsAuthoredBodyDescriptor */
    Result<void> ValidatePhysicsAuthoredBodyDescriptor(const PhysicsAuthoredBodyDescriptor &descriptor) {
        if (descriptor.motion > PhysicsMotionType::Dynamic)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown authored body motion mode."));
        if (descriptor.continuousCollision.mode && (*descriptor.continuousCollision.mode > PhysicsDefaultMotionQuality::LinearCast ||
                                                    (*descriptor.continuousCollision.mode == PhysicsDefaultMotionQuality::LinearCast &&
                                                     descriptor.motion != PhysicsMotionType::Dynamic)))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Explicit linear CCD requires dynamic motion."));
        if (const auto mass = ValidateMassPolicy(descriptor.motion, descriptor.mass); mass.HasError())
            return mass;
        return ValidateInitialVelocity(descriptor.motion, descriptor.initialLinearVelocity, descriptor.initialAngularVelocity,
                                       descriptor.motionSafety);
    }

    /** @copydoc ResolvePhysicsBodyDescriptor */
    Result<PhysicsBodyDescriptor> ResolvePhysicsBodyDescriptor(const PhysicsAuthoredBodyDescriptor &authored, const ShapeHandle shape,
                                                               const PhysicsPose pose, const PhysicsWorldId expectedWorld) {
        if (const auto intent = ValidatePhysicsAuthoredBodyDescriptor(authored); intent.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(intent.ErrorValue());
        PhysicsBodyDescriptor resolved{shape,
                                       pose,
                                       authored.motion,
                                       authored.mass,
                                       authored.initialLinearVelocity,
                                       authored.initialAngularVelocity,
                                       authored.motionSafety,
                                       authored.continuousCollision};
        if (const auto runtime = ValidatePhysicsBodyDescriptor(resolved, expectedWorld); runtime.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(runtime.ErrorValue());
        return Result<PhysicsBodyDescriptor>::Success(std::move(resolved));
    }

    /** @copydoc ValidatePhysicsBodyDescriptor */
    Result<void> ValidatePhysicsBodyDescriptor(const PhysicsBodyDescriptor &descriptor, const PhysicsWorldId expectedWorld) {
        if (const auto owner = ValidatePhysicsHandleOwner(descriptor.shape, expectedWorld); owner.HasError())
            return owner;
        if (const auto pose = ValidatePhysicsPose(descriptor.pose); pose.HasError())
            return pose;
        if (descriptor.motion > PhysicsMotionType::Dynamic)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown body motion mode."));
        if (descriptor.continuousCollision.mode && (*descriptor.continuousCollision.mode > PhysicsDefaultMotionQuality::LinearCast ||
                                                    (*descriptor.continuousCollision.mode == PhysicsDefaultMotionQuality::LinearCast &&
                                                     descriptor.motion != PhysicsMotionType::Dynamic)))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Explicit linear CCD requires dynamic motion."));
        if (const auto mass = ValidateMassPolicy(descriptor.motion, descriptor.mass); mass.HasError())
            return mass;
        return ValidateInitialVelocity(descriptor.motion, descriptor.linearVelocity, descriptor.angularVelocity, descriptor.motionSafety);
    }

    /** @copydoc ValidatePhysicsBodyState */
    Result<void> ValidatePhysicsBodyState(const PhysicsBodyState &state, const PhysicsWorldId expectedWorld) {
        if (const auto owner = ValidatePhysicsHandleOwner(state.body, expectedWorld); owner.HasError())
            return owner;
        if (const auto pose = ValidatePhysicsPose(state.pose); pose.HasError())
            return pose;
        if (state.activity > PhysicsBodyActivity::Static)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown body activity state."));
        if (!FiniteStateVelocity(state))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Observed body velocities must be finite."));
        return Result<void>::Success();
    }

    /** @copydoc ComputePhysicsDampingScale */
    Result<float> ComputePhysicsDampingScale(const float dampingPerSecond, const double deltaSeconds) {
        if (!std::isfinite(dampingPerSecond) || dampingPerSecond < 0.0F || dampingPerSecond > MaximumPhysicsDampingPerSecond ||
            !std::isfinite(deltaSeconds) || deltaSeconds < 0.0)
            return Result<float>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Damping rate and fixed-step duration must be finite and bounded."));
        return Result<float>::Success(static_cast<float>(std::exp(-static_cast<double>(dampingPerSecond) * deltaSeconds)));
    }
}  // namespace Horo::Physics
