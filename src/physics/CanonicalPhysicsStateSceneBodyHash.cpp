#include "CanonicalPhysicsStateBodyControl.h"
#include "CanonicalPhysicsStateTopologyHash.h"

#include <type_traits>

namespace Horo::Physics::Detail {
    /** @copydoc HashStateMaterial */
    void HashStateMaterial(StateDetail::StateHash &hash, const PhysicsQueryMaterial &value) noexcept {
        hash.Id(value.asset);
        hash.Word(value.assetGeneration);
        hash.Word(value.slot.Value());
    }

    /** @copydoc HashStateSurface */
    void HashStateSurface(StateDetail::StateHash &hash, const PhysicsSurfaceEvidence &value) noexcept {
        hash.Id(value.surface);
        hash.Word(value.catalogRevision);
        hash.Digest(value.catalogFingerprint);
        hash.Word(value.collider);
    }

    namespace {
        void Collision(StateDetail::StateHash &hash, const PhysicsSceneCollisionBinding &value) noexcept {
            hash.Id(value.profile);
            hash.Word(value.colliders.size());
            for (const auto &collider : value.colliders) {
                hash.Pose(collider.localPose);
                hash.Word(collider.collider);
                hash.Word(collider.subshape.has_value());
                if (collider.subshape)
                    hash.Word(collider.subshape->Value());
                hash.Word(collider.materials.size());
                for (const auto &material : collider.materials)
                    HashStateMaterial(hash, material);
                hash.Word(collider.surface.has_value());
                if (collider.surface)
                    HashStateSurface(hash, *collider.surface);
            }
        }

        void Policy(StateDetail::StateHash &hash, const PhysicsBodyDescriptor &value) noexcept {
            hash.Pose(value.pose);
            hash.Word(static_cast<std::uint8_t>(value.motion));
            hash.Word(value.mass.index());
            std::visit([&](const auto &mass) {
                using Mass = std::decay_t<decltype(mass)>;
                if constexpr (std::is_same_v<Mass, PhysicsMass>)
                    hash.Float(mass.kilograms);
                else if constexpr (std::is_same_v<Mass, PhysicsDensity>)
                    hash.Float(mass.kilogramsPerCubicMeter);
            }, value.mass);
            hash.Float(value.motionSafety.linearDampingPerSecond);
            hash.Float(value.motionSafety.angularDampingPerSecond);
            hash.Word(static_cast<std::uint8_t>(value.motionSafety.lockedAxes));
            hash.Float(value.motionSafety.maximumLinearSpeed);
            hash.Float(value.motionSafety.maximumAngularSpeed);
            hash.Float(value.motionSafety.maximumDepenetrationSpeed);
            hash.Word(value.continuousCollision.mode.has_value());
            if (value.continuousCollision.mode)
                hash.Word(static_cast<std::uint8_t>(*value.continuousCollision.mode));
            // The retained policy pose is the immutable original construction input; only last velocity intents
            // change through existing body mutations. Current native pose is separately restored solver state.
        }
    }  // namespace

    /** @copydoc HashStateSceneBody */
    Result<void> HashStateSceneBody(StateDetail::StateHash &hash, const CanonicalWorld &world,
                                    const CanonicalPhysicsStateBodyBinding &binding, const CanonicalPhysicsStateBodyRecordIndex &index,
                                    const CanonicalPhysicsStateTopologyMap &map) {
        if (index.index >= world.scene.bodies.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto &record = world.scene.bodies[index.index];
        if (record.handle != binding.body || record.nativeBody.GetIndexAndSequenceNumber() != binding.nativeId)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto shape = FindStateShapeId(map, record.policy.shape);
        const auto *native = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(record.nativeBody);
        if (shape == 0 || !native || native->GetShape() != record.shapeEvidence.GetPtr())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        hash.Word(binding.id);
        hash.Word(shape);
        hash.Digest(record.shapeRecipe);
        hash.Pose(record.pose);
        hash.Word(record.sensor);
        hash.Word(record.motionStorageReserved);
        // Scene entity routing is local to this owner/process. The full persistent-key projection
        // is already bound to binding.id; receiver routing is reissued independently.
        if (!CanonicalBodyConstructionKnown(record.creationControl, record.stateArithmeticControl, record.massArithmeticSubprofile))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        hash.Word(static_cast<std::uint8_t>(record.creationControl->architecture));
        hash.Word(record.creationControl->value);
        hash.Word(static_cast<std::uint8_t>(record.massArithmeticSubprofile->architecture));
        hash.Word(record.massArithmeticSubprofile->value);
        hash.Word(record.collision.has_value());
        if (record.collision)
            Collision(hash, *record.collision);
        Policy(hash, record.policy);
        HashStateNativeBody(hash, *native);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
