#include "CanonicalPhysicsStateEventBindingInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        bool SameCollider(const PhysicsSceneColliderBinding &collider, const PhysicsStateEventEndpoint &endpoint,
                          const std::optional<PhysicsEventMaterial> &material,
                          const std::optional<PhysicsSurfaceEvidence> &surface) noexcept {
            if (collider.collider != endpoint.collider || collider.subshape != endpoint.subshape || collider.surface != surface)
                return false;
            const auto actual = collider.materials.size() == 1 ? std::optional{collider.materials.front()} : std::nullopt;
            return SameStateMaterial(actual, material);
        }

        bool Metadata(const PhysicsSceneCollisionBinding &collision, const PhysicsStateEventEndpoint &endpoint,
                      const std::optional<PhysicsEventMaterial> &material, const std::optional<PhysicsSurfaceEvidence> &surface) noexcept {
            if (collision.colliders.empty())
                return !endpoint.subshape && endpoint.collider == 0 && !material && !surface;
            for (const auto &collider : collision.colliders)
                if (SameCollider(collider, endpoint, material, surface))
                    return true;
            return false;
        }
    }  // namespace

    /** @copydoc BindStateSceneEvent */
    Result<PhysicsEventEndpoint> BindStateSceneEvent(const CanonicalWorld &world, const CanonicalSceneBodyRecord &body,
                                                     const PhysicsStateEventEndpoint &endpoint,
                                                     const std::optional<PhysicsEventMaterial> &material,
                                                     const std::optional<PhysicsSurfaceEvidence> &surface) {
        using Outcome = Result<PhysicsEventEndpoint>;
        if (!body.collision || body.collision->profile != endpoint.profile)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (const auto *profile = world.simulation.Profile(endpoint.profile);
            !profile || !profile->lifecycleEventsEnabled || profile->layer != endpoint.layer)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (!Metadata(*body.collision, endpoint, material, surface))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Outcome::Success({body.handle, body.policy.shape, endpoint.subshape, endpoint.layer, endpoint.profile,
                                 world.simulation.SchemaGeneration(), endpoint.collider});
    }
}  // namespace Horo::Physics::Detail
