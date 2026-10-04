#include "PhysicsWorldInternal.h"

#include <new>

namespace Horo::Physics {
    /** @brief Pins detached native admission while borrowing a world that invalidates it before teardown. */
    struct PhysicsSceneBodyPreparation::Impl final {
        Impl(PhysicsWorld::Impl &owner, Detail::CanonicalSceneBodyBatch batch)
            : owner(&owner), batch(std::move(batch)), identity(owner.identity), revision(owner.publication.Snapshot().publicationRevision),
              thread(owner.runtime->ownerThread) {}

        PhysicsWorld::Impl *owner;
        Detail::CanonicalSceneBodyBatch batch;
        PhysicsWorldId identity;
        std::uint64_t revision;
        std::thread::id thread;
    };

    PhysicsSceneBodyPreparation::PhysicsSceneBodyPreparation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    PhysicsSceneBodyPreparation::~PhysicsSceneBodyPreparation() = default;

    /** @copydoc PhysicsSceneBodyPreparation::Handles */
    std::span<const BodyHandle> PhysicsSceneBodyPreparation::Handles() const noexcept {
        return impl_->batch.Handles();
    }

    /** @copydoc PhysicsSceneBodyPreparation::Shapes */
    std::span<const ShapeHandle> PhysicsSceneBodyPreparation::Shapes() const noexcept {
        return impl_->batch.Shapes();
    }

    /** @copydoc PhysicsSceneBodyPreparation::Constraints */
    std::span<const ConstraintHandle> PhysicsSceneBodyPreparation::Constraints() const noexcept {
        return impl_->batch.Constraints();
    }

    /** @copydoc PhysicsSceneBodyPreparation::RetiredConstraints */
    std::span<const ConstraintHandle> PhysicsSceneBodyPreparation::RetiredConstraints() const noexcept {
        return impl_->batch.RetiredConstraints();
    }

    /** @copydoc PhysicsSceneBodyPreparation::PrepareRetirement */
    Result<void> PhysicsSceneBodyPreparation::PrepareRetirement(const std::span<const BodyHandle> bodies,
                                                                const std::span<const ShapeHandle> shapes,
                                                                const std::span<const ConstraintHandle> constraints) {
        if (const auto valid = ValidatePublication(); valid.HasError())
            return valid;
        try {
            return impl_->batch.PrepareRetirement(bodies, shapes, constraints);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsSceneBodyPreparation::PrepareConstraints */
    Result<void> PhysicsSceneBodyPreparation::PrepareConstraints(const std::span<const PhysicsConstraintDescriptor> descriptors) {
        if (const auto valid = ValidatePublication(); valid.HasError())
            return valid;
        try {
            return impl_->batch.PrepareConstraints(descriptors);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsSceneBodyPreparation::ValidatePublication */
    Result<void> PhysicsSceneBodyPreparation::ValidatePublication() const {
        if (std::this_thread::get_id() != impl_->thread)
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        // Native teardown cancels the batch before deleting owner: never dereference a stale borrowed owner.
        if (auto pending = impl_->batch.ValidatePublication(); pending.HasError())
            return pending;
        const auto &owner = *impl_->owner;
        if (owner.identity != impl_->identity || owner.state != PhysicsWorldState::ActiveSolver || owner.stepping ||
            owner.runtime->state != PhysicsRuntimeState::Ready || owner.publication.Snapshot().publicationRevision != impl_->revision)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneBodyPreparation::Publish */
    void PhysicsSceneBodyPreparation::Publish() noexcept {
        if (!impl_->batch.IsPending())
            return;
        impl_->batch.Publish();
        impl_->owner->InvalidateQueryEventPublication();
    }

    /** @copydoc PhysicsWorld::PrepareSceneBodies */
    Result<std::unique_ptr<PhysicsSceneBodyPreparation>> PhysicsWorld::PrepareSceneBodies(
        const std::span<const PhysicsSceneBodyDescriptor> descriptors) const {
        using Preparation = Result<std::unique_ptr<PhysicsSceneBodyPreparation>>;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Preparation::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Preparation::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return Preparation::Failure(MakeError(PhysicsErrors::InvalidState));
        if (auto capacity = impl_->CheckPublicationRevisionCapacity(); capacity.HasError())
            return Preparation::Failure(capacity.ErrorValue());
        auto prepared = Detail::PrepareCanonicalSceneBodies(impl_->native, impl_->identity, descriptors);
        if (prepared.HasError())
            return Preparation::Failure(prepared.ErrorValue());
        auto state = std::make_unique<PhysicsSceneBodyPreparation::Impl>(*impl_, std::move(prepared).Value());
        return Preparation::Success(std::unique_ptr<PhysicsSceneBodyPreparation>{new PhysicsSceneBodyPreparation{std::move(state)}});
    }

    /** @copydoc PhysicsWorld::PrepareSceneGroup */
    Result<std::unique_ptr<PhysicsSceneBodyPreparation>> PhysicsWorld::PrepareSceneGroup(
        const std::span<const PhysicsSceneGroupShape> shapes, const std::span<const PhysicsSceneGroupBody> bodies) const {
        using Preparation = Result<std::unique_ptr<PhysicsSceneBodyPreparation>>;
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Preparation::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Preparation::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return Preparation::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const auto capacity = impl_->CheckPublicationRevisionCapacity(); capacity.HasError())
            return Preparation::Failure(capacity.ErrorValue());
        try {
            auto prepared = Detail::PrepareCanonicalSceneBodies(impl_->native, impl_->identity, {}, shapes, bodies);
            if (prepared.HasError())
                return Preparation::Failure(prepared.ErrorValue());
            auto state = std::make_unique<PhysicsSceneBodyPreparation::Impl>(*impl_, std::move(prepared).Value());
            return Preparation::Success(std::unique_ptr<PhysicsSceneBodyPreparation>{new PhysicsSceneBodyPreparation{std::move(state)}});
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsWorld::CreateSceneShape */
    Result<ShapeHandle> PhysicsWorld::CreateSceneShape(const PhysicsShapeDescriptor &descriptor) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || Detail::HasPendingCanonicalSceneBodies(impl_->native))
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
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || Detail::HasPendingCanonicalSceneBodies(impl_->native))
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
        if (const Result<void> valid = ValidatePhysicsBodyDescriptor(descriptor.body, impl_->identity); valid.HasError()) {
            impl_->RecordAdmissionDiagnostic(valid.ErrorValue(), descriptor.sceneEntity);
            return Result<BodyHandle>::Failure(valid.ErrorValue());
        }
        if (const auto capacity = impl_->CheckPublicationRevisionCapacity(); capacity.HasError())
            return Result<BodyHandle>::Failure(capacity.ErrorValue());
        auto created = Detail::CreateCanonicalSceneBody(impl_->native, impl_->identity, descriptor);
        if (created.HasValue()) {
            Detail::SetCanonicalSceneEntity(impl_->native, created.Value(), descriptor.sceneEntity);
            impl_->InvalidateQueryEventPublication();
        }
        return created;
    }

    /** @copydoc PhysicsWorld::CreateSceneConstraint */
    Result<ConstraintHandle> PhysicsWorld::CreateSceneConstraint(const PhysicsConstraintDescriptor &descriptor) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || Detail::HasPendingCanonicalSceneBodies(impl_->native))
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
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping || Detail::HasPendingCanonicalSceneBodies(impl_->native))
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsHandleOwner(constraint, impl_->identity); valid.HasError())
            return valid;
        return Detail::DestroyCanonicalSceneConstraint(impl_->native, constraint);
    }

    /** @copydoc PhysicsWorld::ReadSceneActivation */
    Result<PhysicsActivationObservation> PhysicsWorld::ReadSceneActivation() const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsActivationObservation>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsActivationObservation>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->runtime->state != PhysicsRuntimeState::Ready || impl_->stepping)
            return Result<PhysicsActivationObservation>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Detail::ReadCanonicalSceneActivation(impl_->native, impl_->identity);
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
