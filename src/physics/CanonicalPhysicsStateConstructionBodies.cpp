#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateBodyControl.h"
#include "CanonicalPhysicsStateConstructionExport.h"
#include "PhysicsStateSurfaceBinding.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Motion(const JPH::Body &native, PhysicsStateConstructionBody &out) {
            const auto *motion = native.GetMotionPropertiesUnchecked();
            out.hasMotion = motion != nullptr;
            if (!motion)
                return Result<void>::Success();
            out.inverseMass = motion->GetInverseMassUnchecked();
            if (native.IsDynamic()) {
                const auto inertia = motion->GetInverseInertiaDiagonal();
                const auto rotation = motion->GetInertiaRotation();
                out.inverseInertia = {inertia.GetX(), inertia.GetY(), inertia.GetZ()};
                out.inertiaRotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()};
            }
            return Result<void>::Success();
        }

        Result<void> Colliders(const CanonicalSceneBodyRecord &record, CanonicalConstructionRows &rows, PhysicsStateConstructionBody &out) {
            out.colliderBegin = static_cast<std::uint32_t>(rows.colliders);
            if (!record.collision)
                return Result<void>::Success();
            out.profile = record.collision->profile;
            if (record.collision->colliders.size() > rows.storage.colliders.size() - rows.colliders)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            out.colliderCount = static_cast<std::uint32_t>(record.collision->colliders.size());
            for (const auto &c : record.collision->colliders) {
                if (c.materials.size() > rows.storage.materials.size() - rows.materials)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                rows.storage.colliders[rows.colliders++] = {c.localPose,
                                                            c.subshape,
                                                            static_cast<std::uint32_t>(rows.materials),
                                                            static_cast<std::uint32_t>(c.materials.size()),
                                                            StateDetail::CaptureStateSurface(c.surface),
                                                            c.collider};
                std::copy(c.materials.begin(), c.materials.end(), rows.storage.materials.begin() + rows.materials);
                rows.materials += c.materials.size();
            }
            return Result<void>::Success();
        }

        Result<void> Body(const CanonicalWorld &world, const CanonicalPhysicsStateBodyBinding &binding,
                          const CanonicalPhysicsStateBodyRecordIndex &index, const CanonicalPhysicsStateTopologyMap &map,
                          CanonicalConstructionRows &rows) {
            if (index.index >= world.scene.bodies.size() || rows.bodies >= rows.storage.bodies.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const auto &record = world.scene.bodies[index.index];
            if (record.handle != binding.body ||
                !CanonicalBodyConstructionKnown(record.creationControl, record.stateArithmeticControl, record.massArithmeticSubprofile))
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            const auto *native = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(record.nativeBody);
            if (!native || record.nativeBody.GetIndexAndSequenceNumber() != binding.nativeId)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            PhysicsStateConstructionBody out{.id = binding.id,
                                             .shape = FindStateShapeId(map, record.policy.shape),
                                             .originalPose = record.policy.pose,
                                             .policy = {.motion = record.policy.motion,
                                                        .mass = record.policy.mass,
                                                        .motionSafety = record.policy.motionSafety,
                                                        .continuousCollision = record.policy.continuousCollision},
                                             .sensor = record.sensor,
                                             .motionStorageReserved = record.motionStorageReserved,
                                             .floatingPoint = *record.creationControl,
                                             .massArithmeticSubprofile = *record.massArithmeticSubprofile};
            if (auto motion = Motion(*native, out); motion.HasError())
                return motion;
            if (auto colliders = Colliders(record, rows, out); colliders.HasError())
                return colliders;
            rows.storage.bodies[rows.bodies++] = out;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExportCanonicalConstructionBodies */
    Result<void> ExportCanonicalConstructionBodies(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                   const CanonicalPhysicsStateTopologyMap &map, CanonicalConstructionRows &rows) {
        if (map.bodies.size() != bindings.stableBodies.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        for (std::size_t i = 0; i < map.bodies.size(); ++i) {
            if (map.bodies[i].fixture)
                continue;
            if (auto copied = Body(world, bindings.stableBodies[i], map.bodies[i], map, rows); copied.HasError())
                return copied;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
