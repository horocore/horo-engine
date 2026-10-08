#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Separates static bodies from the moving-body activation lifecycle. */
        [[nodiscard]] PhysicsBodyActivity ReadNativeActivity(const JPH::Body &body) noexcept {
            using enum PhysicsBodyActivity;
            if (body.IsStatic())
                return Static;
            return body.IsActive() ? Awake : Sleeping;
        }

        /** @brief Translates the solver's observed motion mode to the Horo policy enum. */
        [[nodiscard]] Result<PhysicsMotionType> ReadNativeMotion(const JPH::Body &native) {
            switch (native.GetMotionType()) {
                case JPH::EMotionType::Static:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Static);
                case JPH::EMotionType::Kinematic:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Kinematic);
                case JPH::EMotionType::Dynamic:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Dynamic);
                default:
                    return Result<PhysicsMotionType>::Failure(
                        MakeError(PhysicsErrors::SolverFatalCondition, "Native body has an unknown motion mode."));
            }
        }

        /** @brief Reject unknown native quality instead of presenting a fallback CCD mode. */
        [[nodiscard]] Result<PhysicsDefaultMotionQuality> ReadNativeMotionQuality(const JPH::Body &native) {
            if (native.IsStatic())
                return Result<PhysicsDefaultMotionQuality>::Success(PhysicsDefaultMotionQuality::Discrete);
            switch (native.GetMotionProperties()->GetMotionQuality()) {
                case JPH::EMotionQuality::Discrete:
                    return Result<PhysicsDefaultMotionQuality>::Success(PhysicsDefaultMotionQuality::Discrete);
                case JPH::EMotionQuality::LinearCast:
                    return Result<PhysicsDefaultMotionQuality>::Success(PhysicsDefaultMotionQuality::LinearCast);
            }
            return Result<PhysicsDefaultMotionQuality>::Failure(MakeError(PhysicsErrors::SolverFatalCondition));
        }

        /** @brief Rejects non-finite body evidence before returning an owned snapshot. */
        [[nodiscard]] bool FiniteReconciliation(const PhysicsBodyReconciliation &value) noexcept {
            const std::array finite{Math::IsFinite(value.state.pose.translation), Math::IsFinite(value.state.pose.rotation),
                                    Math::IsFinite(value.state.linearVelocity), Math::IsFinite(value.state.angularVelocity),
                                    Math::IsFinite(value.observedBoundsExtent)};
            return std::ranges::all_of(finite, std::identity{});
        }

        /** @brief Reads dynamic inverse mass without inventing mass for locked translation. */
        [[nodiscard]] Result<std::optional<float>> ReadNativeMass(const JPH::Body &native, const PhysicsMotionType motion) {
            if (motion != PhysicsMotionType::Dynamic)
                return Result<std::optional<float>>::Success(std::nullopt);
            const float inverseMass = native.GetMotionProperties()->GetInverseMass();
            if (!std::isfinite(inverseMass) || inverseMass < 0.0F)
                return Result<std::optional<float>>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Native dynamic body has invalid mass."));
            return Result<std::optional<float>>::Success(inverseMass > 0.0F ? std::optional<float>{1.0F / inverseMass} : std::nullopt);
        }
    }  // namespace

    /** @copydoc ReadCanonicalSceneBodyReconciliation */
    Result<PhysicsBodyReconciliation> ReadCanonicalSceneBodyReconciliation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                           const BodyHandle body) {
        if (const std::array valid{world.value != nullptr, owner.IsValid()}; !std::ranges::all_of(valid, std::identity{}))
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(body, owner); handle.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(handle.ErrorValue());
        const auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto record = std::ranges::find_if(canonical.scene.bodies, [body](const auto &candidate) {
            return candidate.handle == body;
        });
        if (record == canonical.scene.bodies.end())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::HandleStale));
        JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), record->nativeBody);
        if (!lock.Succeeded())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::HandleStale));
        const JPH::Body &native = lock.GetBody();
        const auto shape = std::ranges::find_if(canonical.scene.shapes, [&native](const auto &candidate) {
            return candidate.shape.GetPtr() == native.GetShape();
        });
        if (shape == canonical.scene.shapes.end())
            return Result<PhysicsBodyReconciliation>::Failure(
                MakeError(PhysicsErrors::SolverFatalCondition, "Native body shape has no resident Horo identity."));
        const auto position = native.GetPosition();
        const auto rotation = native.GetRotation();
        const auto linear = native.GetLinearVelocity();
        const auto angular = native.GetAngularVelocity();
        const auto boundsExtent = native.GetWorldSpaceBounds().GetSize();
        const auto motion = ReadNativeMotion(native);
        if (motion.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(motion.ErrorValue());
        const auto mass = ReadNativeMass(native, motion.Value());
        if (mass.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(mass.ErrorValue());
        const auto quality = ReadNativeMotionQuality(native);
        if (quality.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(quality.ErrorValue());
        PhysicsBodyReconciliation result{.policy = record->policy,
                                         .state = {.body = body,
                                                   .pose = {.translation = {position.GetX(), position.GetY(), position.GetZ()},
                                                            .rotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(),
                                                                         rotation.GetW()}},
                                                   .linearVelocity = {linear.GetX(), linear.GetY(), linear.GetZ()},
                                                   .angularVelocity = {angular.GetX(), angular.GetY(), angular.GetZ()},
                                                   .activity = ReadNativeActivity(native)},
                                         .observedMotion = motion.Value(),
                                         .observedShape = shape->handle,
                                         .observedMassKilograms = mass.Value(),
                                         .observedBoundsExtent = {boundsExtent.GetX(), boundsExtent.GetY(), boundsExtent.GetZ()},
                                         .observedMotionQuality = quality.Value()};
        if (!FiniteReconciliation(result))
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::BodyStateNonFinite));
        return Result<PhysicsBodyReconciliation>::Success(std::move(result));
    }

    /** @copydoc ReadCanonicalSceneActivation */
    Result<PhysicsActivationObservation> ReadCanonicalSceneActivation(const CanonicalWorldHandle world, const PhysicsWorldId owner) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsActivationObservation>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        PhysicsActivationObservation result{.world = owner};
        for (const auto &record : canonical.scene.bodies) {
            JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), record.nativeBody);
            if (!lock.Succeeded())
                return Result<PhysicsActivationObservation>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto &body = lock.GetBody();
            if (body.IsStatic())
                ++result.staticBodies;
            else if (body.IsActive())
                ++result.awakeMovingBodies;
            else
                ++result.sleepingMovingBodies;
        }
        return Result<PhysicsActivationObservation>::Success(result);
    }

    /** @copydoc ReadCanonicalSceneBodyPolicy */
    Result<PhysicsBodyDescriptor> ReadCanonicalSceneBodyPolicy(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                               const BodyHandle body) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(body, owner); handle.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(handle.ErrorValue());
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        const auto record = std::ranges::find_if(canonical.scene.bodies, [body](const auto &candidate) {
            return candidate.handle == body;
        });
        if (record == canonical.scene.bodies.end())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<PhysicsBodyDescriptor>::Success(record->policy);
    }

    /** @copydoc ReadCanonicalSceneJointState */
    Result<PhysicsJointState> ReadCanonicalSceneJointState(const CanonicalWorldHandle world, const ConstraintHandle constraint) {
        if (world.value == nullptr)
            return Result<PhysicsJointState>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        const auto found = std::ranges::find_if(canonical.scene.constraints, [constraint](const auto &record) {
            return record.handle == constraint;
        });
        if (found == canonical.scene.constraints.end())
            return Result<PhysicsJointState>::Failure(MakeError(PhysicsErrors::HandleStale));
        switch (found->constraint->GetSubType()) {
            case JPH::EConstraintSubType::Hinge:
                return Result<PhysicsJointState>::Success(
                    {PhysicsJointCoordinateKind::AngleRadians,
                     static_cast<const JPH::HingeConstraint *>(found->constraint.GetPtr())->GetCurrentAngle()});
            case JPH::EConstraintSubType::Slider:
                return Result<PhysicsJointState>::Success(
                    {PhysicsJointCoordinateKind::PositionMeters,
                     static_cast<const JPH::SliderConstraint *>(found->constraint.GetPtr())->GetCurrentPosition()});
            default:
                return Result<PhysicsJointState>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Only hinge and slider joints expose a single-axis coordinate."));
        }
    }
}  // namespace Horo::Physics::Detail
