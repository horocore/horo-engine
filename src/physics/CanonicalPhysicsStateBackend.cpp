#include "CanonicalPhysicsStateBackendInternal.h"

#include <limits>

namespace Horo::Physics::Detail {
    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateRestoredOwnerSwap */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateRestoredOwnerSwap(CanonicalWorldHandle previous,
                                                                           CanonicalWorldHandle restored) const {
        if (impl_->owner.value != previous.value || impl_->candidate.get() != restored.value || !previous.value || !restored.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (auto epochs = ValidateOwnerEpochs(previous); epochs.HasError())
            return epochs;
        if (const auto &candidateBinding = static_cast<const CanonicalWorld *>(restored.value)->simulation.binding;
            candidateBinding.generation != impl_->filterEpoch || candidateBinding.surfaces.mappingGeneration != impl_->surfaceEpoch)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (const auto &source = *static_cast<const CanonicalWorld *>(previous.value);
            source.continuousCollision.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        return Result<void>::Success();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::AdoptRestoredOwner */
    void CanonicalPhysicsStateWorkspace::AdoptRestoredOwner(CanonicalWorldHandle previous, CanonicalWorldHandle restored) noexcept {
        if (impl_->owner.value != previous.value || impl_->candidate.get() != restored.value || !restored.value)
            std::abort();
        const auto &source = *static_cast<const CanonicalWorld *>(previous.value);
        if (source.continuousCollision.revision == std::numeric_limits<std::uint64_t>::max())
            std::abort();
        // Public identity/nonreuse and query-generation authority belongs to the current receiving owner,
        // not the historical solver checkpoint or the candidate's cold construction time.
        impl_->candidate->nextResourceSlot = source.nextResourceSlot;
        impl_->candidate->scene.nextConstraintSlot = source.scene.nextConstraintSlot;
        impl_->candidate->query.nextFixtureGeneration = source.query.nextFixtureGeneration;
        impl_->candidate->query.querySchemaGeneration = source.query.querySchemaGeneration;
        impl_->candidate->continuousCollision.revision = source.continuousCollision.revision + 1;
        (void)impl_->candidate.release();
        impl_->candidate.reset(static_cast<CanonicalWorld *>(previous.value));
        impl_->owner = restored;
        // Aggregate std::swap move-constructs four vectors, allocating MSVC debug iterator proxies.
        // Exchange the retained buffers directly so publication stays allocation-free and non-throwing.
        auto &ownerMap = impl_->ownerMap;
        auto &candidateMap = impl_->candidateMap;
        ownerMap.bodies.swap(candidateMap.bodies);
        ownerMap.joints.swap(candidateMap.joints);
        ownerMap.shapesByHandle.swap(candidateMap.shapesByHandle);
        ownerMap.shapeRecords.swap(candidateMap.shapeRecords);
        std::swap(ownerMap.sceneBodies, candidateMap.sceneBodies);
        std::swap(ownerMap.queryBodies, candidateMap.queryBodies);
        std::swap(ownerMap.sceneShapes, candidateMap.sceneShapes);
        std::swap(ownerMap.sceneJoints, candidateMap.sceneJoints);
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::CanonicalPhysicsStateWorkspace */
    CanonicalPhysicsStateWorkspace::CanonicalPhysicsStateWorkspace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    CanonicalPhysicsStateWorkspace::~CanonicalPhysicsStateWorkspace() = default;

    /** @copydoc CanonicalPhysicsStateWorkspace::Capture */
    Result<PhysicsStateSolverView> CanonicalPhysicsStateWorkspace::Capture(CanonicalWorldHandle world,
                                                                           StateDetail::SolverStorage &storage) {
        if (world.value != impl_->owner.value)
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        return CaptureCanonicalPhysicsState(world, impl_->settings, impl_->bindings.View(), storage, impl_->CaptureScratch());
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Restore */
    Result<CanonicalWorldHandle> CanonicalPhysicsStateWorkspace::Restore(CanonicalWorldHandle world, const PhysicsStateSolverView &state,
                                                                         const PhysicsContinuousCollisionPolicy &continuousCollision) {
        if (world.value != impl_->owner.value)
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        return PrepareCanonicalPhysicsStateRestore(world, {impl_->candidate.get()}, impl_->settings, impl_->bindings.View(), state,
                                                   continuousCollision, impl_->RestoreScratch());
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateOwnerEpochs */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateOwnerEpochs(CanonicalWorldHandle world) const {
        if (world.value != impl_->owner.value || !world.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto &binding = static_cast<const CanonicalWorld *>(world.value)->simulation.binding;
            binding.generation != impl_->filterEpoch || binding.surfaces.mappingGeneration != impl_->surfaceEpoch)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return Result<void>::Success();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::TopologyFingerprint */
    Result<Sha256Digest> CanonicalPhysicsStateWorkspace::TopologyFingerprint(CanonicalWorldHandle world) const {
        if (auto epochs = ValidateOwnerEpochs(world); epochs.HasError())
            return Result<Sha256Digest>::Failure(epochs.ErrorValue());
        return CanonicalPhysicsStateTopologyFingerprint(*static_cast<const CanonicalWorld *>(world.value), impl_->bindings.View(),
                                                        impl_->ownerMap);
    }

    Sha256Digest CanonicalPhysicsStateWorkspace::FilterFingerprint() const noexcept {
        return impl_->filters;
    }

    Sha256Digest CanonicalPhysicsStateWorkspace::SurfaceFingerprint() const noexcept {
        return impl_->surfaces;
    }

    const StateDetail::SolverStorage &CanonicalPhysicsStateWorkspace::RecaptureStorage() const noexcept {
        return *impl_->recapture;
    }

    std::span<const PhysicsStateShapeBinding> CanonicalPhysicsStateWorkspace::Shapes() const noexcept {
        return impl_->shapes;
    }

    std::span<const PhysicsStateBodyBinding> CanonicalPhysicsStateWorkspace::Bodies() const noexcept {
        return impl_->bodies;
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::BodyId */
    PhysicsStateObjectId CanonicalPhysicsStateWorkspace::BodyId(BodyHandle body) const noexcept {
        const auto &values = impl_->bodiesByHandle;
        const auto found = std::ranges::lower_bound(values, body, {}, &PhysicsStateBodyBinding::body);
        return found != values.end() && found->body == body ? found->id : 0;
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ShapeId */
    PhysicsStateObjectId CanonicalPhysicsStateWorkspace::ShapeId(ShapeHandle shape) const noexcept {
        return FindStateShapeId(impl_->ownerMap, shape);
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::SolverWireScratch */
    std::span<std::byte> CanonicalPhysicsStateWorkspace::SolverWireScratch() noexcept {
        return impl_->expectedWire;
    }

    std::shared_ptr<const CanonicalPhysicsStateResources> CanonicalPhysicsStateWorkspace::Resources() const noexcept {
        return impl_->resources;
    }

    std::size_t CanonicalPhysicsStateWorkspace::MaximumSolverWireBytes() const noexcept {
        return impl_->expectedWire.size();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ConstructionBytes */
    std::span<const std::byte> CanonicalPhysicsStateWorkspace::ConstructionBytes() const noexcept {
        return impl_->construction ? impl_->construction->Bytes() : std::span<const std::byte>{};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::RetainedPayloadBytes */
    std::size_t CanonicalPhysicsStateWorkspace::RetainedPayloadBytes() const noexcept {
        const auto &value = *impl_;
        return sizeof(*this) + sizeof(value) + value.shapes.capacity() * sizeof(PhysicsStateShapeBinding) +
               (value.bodies.capacity() + value.bodiesByHandle.capacity()) * sizeof(PhysicsStateBodyBinding) +
               (value.bindings.nativeBodies.capacity() + value.bindings.stableBodies.capacity()) *
                   sizeof(CanonicalPhysicsStateBodyBinding) +
               (value.bindings.nativeJoints.capacity() + value.bindings.stableJoints.capacity()) *
                   sizeof(CanonicalPhysicsStateJointBinding) +
               value.ownerMap.RetainedPayloadBytes() + value.candidateMap.RetainedPayloadBytes() + value.candidateReservation +
               value.recapture->RetainedPayloadBytes() + (value.construction ? value.construction->RetainedPayloadBytes() : 0) +
               value.temporary.capacity() + value.recorder.capacity() + value.expectedWire.capacity() + value.actualWire.capacity() +
               value.active.capacity() * sizeof(std::uint32_t);
    }

    /** @copydoc CanonicalPhysicsStateResourceBytes */
    std::size_t CanonicalPhysicsStateResourceBytes(const std::shared_ptr<const CanonicalPhysicsStateResources> &resources) noexcept {
        return resources ? resources->retainedBytes : 0;
    }
}  // namespace Horo::Physics::Detail
