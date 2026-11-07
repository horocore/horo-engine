#include "CanonicalPhysicsStateRestartBindings.h"
#include "PhysicsStateSurfaceBinding.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<std::optional<PhysicsSceneCollisionBinding>> Collision(const PhysicsStateConstructionManifest &manifest,
                                                                      const PhysicsStateConstructionBody &body,
                                                                      const PhysicsSurfaceWorldBinding &surfaces) {
            using Outcome = Result<std::optional<PhysicsSceneCollisionBinding>>;
            if (!body.profile)
                return Outcome::Success({});
            PhysicsSceneCollisionBinding binding{.profile = *body.profile};
            binding.colliders.reserve(body.colliderCount);
            for (const auto &c : manifest.colliders.subspan(body.colliderBegin, body.colliderCount)) {
                auto surface = StateDetail::RebindStateSurface(surfaces, c.surface);
                if (surface.HasError())
                    return Outcome::Failure(surface.ErrorValue());
                PhysicsSceneColliderBinding collider{.localPose = c.localPose,
                                                     .subshape = c.subshape,
                                                     .surface = surface.Value(),
                                                     .collider = c.collider};
                const auto materials = manifest.materials.subspan(c.materialBegin, c.materialCount);
                collider.materials.assign(materials.begin(), materials.end());
                binding.colliders.push_back(std::move(collider));
            }
            return Outcome::Success(std::move(binding));
        }

        Result<void> ExactMass(CanonicalWorld &world, BodyHandle handle, const PhysicsStateConstructionBody &source) {
            auto record = std::ranges::find(world.scene.bodies, handle, &CanonicalSceneBodyRecord::handle);
            if (record == world.scene.bodies.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            JPH::BodyLockWrite lock(world.native.system->GetBodyLockInterfaceNoLock(), record->nativeBody);
            if (!lock.Succeeded())
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            auto *motion = lock.GetBody().GetMotionPropertiesUnchecked();
            if ((motion != nullptr) != source.hasMotion)
                return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
            if (motion) {
                motion->SetInverseMass(source.inverseMass);
                if (lock.GetBody().IsDynamic())
                    motion->SetInverseInertia(JPH::Vec3(source.inverseInertia[0], source.inverseInertia[1], source.inverseInertia[2]),
                                              JPH::Quat(source.inertiaRotation[0], source.inertiaRotation[1], source.inertiaRotation[2],
                                                        source.inertiaRotation[3]));
            }
            return Result<void>::Success();
        }

        ShapeHandle Shape(const CanonicalRestartBindings &bindings, PhysicsStateObjectId id) noexcept {
            const auto found = std::ranges::lower_bound(bindings.shapes, id, {}, &PhysicsStateShapeBinding::id);
            return found != bindings.shapes.end() && found->id == id ? found->shape : ShapeHandle{};
        }

        Result<BodyHandle> Create(CanonicalWorldHandle handle, PhysicsWorldId owner, const PhysicsStateConstructionManifest &manifest,
                                  const PhysicsStateConstructionBody &source, const CanonicalRestartBodyRoute &route,
                                  const CanonicalRestartBindings &bindings) {
            using Outcome = Result<BodyHandle>;
            const auto shape = Shape(bindings, source.shape);
            if (!shape.IsValid() || route.body != source.id)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto &world = *static_cast<CanonicalWorld *>(handle.value);
            auto collision = Collision(manifest, source, world.simulation.binding.surfaces);
            if (collision.HasError())
                return Outcome::Failure(collision.ErrorValue());
            PhysicsSceneBodyDescriptor descriptor{.body = {.shape = shape,
                                                           .pose = source.originalPose,
                                                           .motion = source.policy.motion,
                                                           .mass = source.policy.mass,
                                                           .motionSafety = source.policy.motionSafety,
                                                           .continuousCollision = source.policy.continuousCollision},
                                                  .sensor = source.sensor,
                                                  .sceneEntity = route.sceneEntity,
                                                  .collision = std::move(collision).Value()};
            auto created = CreateCanonicalSceneBody(handle, owner, descriptor);
            if (created.HasError())
                return created;
            if (auto restored = ExactMass(world, created.Value(), source); restored.HasError())
                return Outcome::Failure(restored.ErrorValue());
            return created;
        }
    }  // namespace

    /** @copydoc CreateCanonicalRestartBodies */
    Result<void> CreateCanonicalRestartBodies(CanonicalWorldHandle world, PhysicsWorldId owner,
                                              const PhysicsStateConstructionManifest &manifest,
                                              std::span<const CanonicalRestartBodyRoute> routes, CanonicalRestartBindings &bindings,
                                              const CancellationToken &cancellation) {
        if (!world.value || routes.size() != manifest.bodies.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        bindings.bodies.reserve(manifest.bodies.size() + manifest.fixtures.size());
        for (std::size_t index = 0; index < manifest.bodies.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
            auto created = Create(world, owner, manifest, manifest.bodies[index], routes[index], bindings);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            bindings.bodies.push_back({manifest.bodies[index].id, created.Value()});
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
