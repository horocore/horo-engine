#include "CanonicalPhysicsStateRestartBindings.h"

namespace Horo::Physics::Detail {
    namespace {
        BodyHandle Body(const CanonicalRestartBindings &bindings, PhysicsStateObjectId id) noexcept {
            const auto found = std::ranges::lower_bound(bindings.bodies, id, {}, &PhysicsStateBodyBinding::id);
            return found != bindings.bodies.end() && found->id == id ? found->body : BodyHandle{};
        }

        Result<PhysicsConstraintDescriptor> Descriptor(const PhysicsStateConstructionJoint &source,
                                                       const CanonicalRestartBindings &bindings, PhysicsWorldId owner) {
            PhysicsConstraintDescriptor descriptor;
            descriptor.first = {Body(bindings, source.first), source.firstLocalFrame};
            if (source.second)
                descriptor.second = PhysicsBodyAnchor{Body(bindings, *source.second), source.secondLocalOrWorldFrame};
            else
                descriptor.second = PhysicsWorldAnchor{source.secondLocalOrWorldFrame};
            descriptor.parameters = source.parameters;
            descriptor.collisionPolicy = source.collisionPolicy;
            if (auto valid = ValidatePhysicsConstraintDescriptor(descriptor, owner); valid.HasError())
                return Result<PhysicsConstraintDescriptor>::Failure(valid.ErrorValue());
            return Result<PhysicsConstraintDescriptor>::Success(std::move(descriptor));
        }

        Result<ConstraintHandle> Create(CanonicalWorldHandle world, PhysicsWorldId owner, const PhysicsStateConstructionJoint &source,
                                        const CanonicalRestartBindings &bindings) {
            auto descriptor = Descriptor(source, bindings, owner);
            if (descriptor.HasError())
                return Result<ConstraintHandle>::Failure(descriptor.ErrorValue());
            const CanonicalConstraintCreation creation{source.firstConstructorFrame,
                                                       source.secondConstructorFrame,
                                                       {source.firstCom.position, source.firstCom.rotation},
                                                       {source.secondCom.position, source.secondCom.rotation},
                                                       source.floatingPoint};
            auto prepared = PrepareCanonicalRestartConstraintRecord(world, owner, descriptor.Value(), creation);
            if (prepared.HasError())
                return Result<ConstraintHandle>::Failure(prepared.ErrorValue());
            return PublishCanonicalConstraintRecord(*static_cast<CanonicalWorld *>(world.value), std::move(prepared).Value());
        }
    }  // namespace

    /** @copydoc CreateCanonicalRestartJoints */
    Result<void> CreateCanonicalRestartJoints(CanonicalWorldHandle world, PhysicsWorldId owner,
                                              const PhysicsStateConstructionManifest &manifest, CanonicalRestartBindings &bindings,
                                              const CancellationToken &cancellation) {
        if (!world.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        bindings.constraints.reserve(manifest.joints.size());
        for (const auto &source : manifest.joints) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
            auto created = Create(world, owner, source, bindings);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            bindings.constraints.push_back({source.id, created.Value()});
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
