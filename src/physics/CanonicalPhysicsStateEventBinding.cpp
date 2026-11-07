#include "CanonicalPhysicsStateEventBindingInternal.h"
#include "PhysicsStateSurfaceBinding.h"

namespace Horo::Physics::Detail {
    /** @copydoc SameStateMaterial */
    bool SameStateMaterial(const std::optional<PhysicsQueryMaterial> &actual,
                           const std::optional<PhysicsEventMaterial> &material) noexcept {
        if (actual.has_value() != material.has_value())
            return false;
        return !actual ||
               (actual->asset == material->asset && actual->assetGeneration == material->assetGeneration && actual->slot == material->slot);
    }

    namespace {
        Result<PhysicsEventEndpoint> ResolveMapped(const CanonicalPhysicsStateWorkspace &workspace, const CanonicalWorld &world,
                                                   const CanonicalPhysicsStateBodyRecordIndex &record, BodyHandle expectedBody,
                                                   const PhysicsStateEventEndpoint &endpoint,
                                                   const std::optional<PhysicsEventMaterial> &material,
                                                   const std::optional<PhysicsSurfaceEvidence> &surface) {
            using Outcome = Result<PhysicsEventEndpoint>;
            if (record.fixture) {
                if (record.index >= world.query.fixtures.size())
                    return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
                const auto &fixture = world.query.fixtures[record.index];
                if (fixture.fixture.body != expectedBody || workspace.ShapeId(fixture.fixture.shape) != endpoint.shape)
                    return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                return BindStateFixtureEvent(world, fixture, endpoint, material, surface);
            }
            if (record.index >= world.scene.bodies.size())
                return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
            const auto &body = world.scene.bodies[record.index];
            if (body.handle != expectedBody || workspace.ShapeId(body.policy.shape) != endpoint.shape)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return BindStateSceneEvent(world, body, endpoint, material, surface);
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateWorkspace::ResolveEventEndpoint */
    Result<PhysicsEventEndpoint> CanonicalPhysicsStateWorkspace::ResolveEventEndpoint(
        const PhysicsStateEventEndpoint &endpoint, const std::optional<PhysicsEventMaterial> &material,
        const std::optional<PhysicsStateSurfaceEvidence> &savedSurface) const {
        using Outcome = Result<PhysicsEventEndpoint>;
        auto surface = ResolveStateSurface(savedSurface);
        if (surface.HasError())
            return Outcome::Failure(surface.ErrorValue());
        const auto &bindings = impl_->bindings.stableBodies;
        const auto found = std::ranges::lower_bound(bindings, endpoint.body, {}, &CanonicalPhysicsStateBodyBinding::id);
        if (found == bindings.end() || found->id != endpoint.body || !impl_->owner.value)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        const auto offset = static_cast<std::size_t>(found - bindings.begin());
        if (offset >= impl_->ownerMap.bodies.size())
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return ResolveMapped(*this, *static_cast<const CanonicalWorld *>(impl_->owner.value), impl_->ownerMap.bodies[offset], found->body,
                             endpoint, material, surface.Value());
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ResolveStateSurface */
    Result<std::optional<PhysicsSurfaceEvidence>> CanonicalPhysicsStateWorkspace::ResolveStateSurface(
        const std::optional<PhysicsStateSurfaceEvidence> &saved) const {
        if (auto epochs = ValidateOwnerEpochs(impl_->owner); epochs.HasError())
            return Result<std::optional<PhysicsSurfaceEvidence>>::Failure(epochs.ErrorValue());
        const auto &world = *static_cast<const CanonicalWorld *>(impl_->owner.value);
        return StateDetail::RebindStateSurface(world.simulation.binding.surfaces, saved);
    }
}  // namespace Horo::Physics::Detail
