#include "PhysicsWorldInternal.h"

namespace Horo::Physics {
    /** @copydoc PhysicsWorld::CreateSceneShape */
    Result<ShapeHandle> PhysicsWorld::CreateSceneShape(const PhysicsShapeDescriptor &descriptor) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsShapeDescriptor(descriptor); valid.HasError())
            return Result<ShapeHandle>::Failure(valid.ErrorValue());
        return Detail::CreateCanonicalSceneShape(impl_->native, impl_->identity, descriptor);
    }

    /** @copydoc PhysicsWorld::CreateSceneCompoundShape */
    Result<ShapeHandle> PhysicsWorld::CreateSceneCompoundShape(const std::span<const PhysicsSceneShapeInstance> instances) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (instances.empty())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A scene compound shape requires a child shape."));
        for (const PhysicsSceneShapeInstance &instance : instances) {
            if (const Result<void> owner = ValidatePhysicsHandleOwner(instance.shape, impl_->identity); owner.HasError())
                return Result<ShapeHandle>::Failure(owner.ErrorValue());
            if (const Result<void> pose = ValidatePhysicsPose(instance.localPose); pose.HasError())
                return Result<ShapeHandle>::Failure(pose.ErrorValue());
        }
        return Detail::CreateCanonicalSceneCompoundShape(impl_->native, impl_->identity, instances);
    }

    /** @copydoc PhysicsWorld::CreateSceneBody */
    Result<BodyHandle> PhysicsWorld::CreateSceneBody(const PhysicsSceneBodyDescriptor &descriptor) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsBodyDescriptor(descriptor.body, impl_->identity); valid.HasError())
            return Result<BodyHandle>::Failure(valid.ErrorValue());
        return Detail::CreateCanonicalSceneBody(impl_->native, impl_->identity, descriptor);
    }

    /** @copydoc PhysicsWorld::CreateSceneConstraint */
    Result<ConstraintHandle> PhysicsWorld::CreateSceneConstraint(const PhysicsConstraintDescriptor &descriptor) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsConstraintDescriptor(descriptor, impl_->identity); valid.HasError())
            return Result<ConstraintHandle>::Failure(valid.ErrorValue());
        if (std::holds_alternative<PhysicsBodyAnchor>(descriptor.second) &&
            descriptor.collisionPolicy == PhysicsJointCollisionPolicy::AllowBetweenBodies)
            return Result<ConstraintHandle>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported,
                          "CanonicalV1 scene collision layers are closed; joints cannot enable contacts between their body endpoints."));
        return Detail::CreateCanonicalSceneConstraint(impl_->native, impl_->identity, descriptor);
    }

    /** @copydoc PhysicsWorld::DestroySceneConstraint */
    Result<void> PhysicsWorld::DestroySceneConstraint(const ConstraintHandle constraint) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsHandleOwner(constraint, impl_->identity); valid.HasError())
            return valid;
        return Detail::DestroyCanonicalSceneConstraint(impl_->native, constraint);
    }

    /** @copydoc PhysicsWorld::ReadSceneJointState */
    Result<PhysicsJointState> PhysicsWorld::ReadSceneJointState(const ConstraintHandle constraint) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsJointState>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsJointState>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsJointState>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsHandleOwner(constraint, impl_->identity); valid.HasError())
            return Result<PhysicsJointState>::Failure(valid.ErrorValue());
        return Detail::ReadCanonicalSceneJointState(impl_->native, constraint);
    }
}  // namespace Horo::Physics
