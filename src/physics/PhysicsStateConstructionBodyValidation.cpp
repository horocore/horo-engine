#include "PhysicsStateConstructionManifestInternal.h"

#include <cmath>

namespace Horo::Physics::StateDetail {
    bool ConstructionMaterialValid(const PhysicsQueryMaterial &v) noexcept {
        return v.asset.IsValid() && v.assetGeneration != 0 && v.slot.IsValid();
    }

    bool ConstructionSurfaceValid(const PhysicsStateSurfaceEvidence &v) noexcept {
        return v.surface.IsValid() && v.catalogRevision != 0 && std::ranges::any_of(v.catalogFingerprint.bytes, [](auto byte) {
            return byte != 0;
        });
    }

    namespace {
        bool InertiaValid(const PhysicsStateConstructionBody &b) noexcept {
            if (!std::isfinite(b.inverseMass) || b.inverseMass < 0)
                return false;
            if (!b.hasMotion)
                return b.inverseMass == 0 && Zero(b.inverseInertia) && Zero(b.inertiaRotation);
            if (b.policy.motion != PhysicsMotionType::Dynamic)
                return Zero(b.inverseInertia) && Zero(b.inertiaRotation);
            return UnitRotation(b.inertiaRotation) && std::ranges::all_of(b.inverseInertia, [](float value) {
                return std::isfinite(value) && value >= 0;
            });
        }

        bool MotionStorageValid(const PhysicsStateConstructionBody &b) noexcept {
            return b.hasMotion == (b.motionStorageReserved || b.policy.motion != PhysicsMotionType::Static);
        }

        bool ColliderSurfaceValid(const PhysicsStateConstructionCollider &collider) noexcept {
            if (!collider.surface)
                return true;
            return ConstructionSurfaceValid(*collider.surface) && collider.surface->collider == collider.collider &&
                   collider.materialCount == 1;
        }

        Result<void> Collider(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionCollider &c,
                              std::size_t &nextMaterial) {
            if (!c.collider || c.materialBegin != nextMaterial || c.materialBegin > m.materials.size() ||
                c.materialCount > m.materials.size() - c.materialBegin)
                return MalformedConstruction();
            if (auto pose = ValidatePhysicsPose(c.localPose); pose.HasError())
                return pose;
            if (c.subshape && !c.subshape->IsValid())
                return MalformedConstruction();
            if (!ColliderSurfaceValid(c))
                return MalformedConstruction();
            nextMaterial += c.materialCount;
            return Result<void>::Success();
        }

        bool UniqueColliders(std::span<const PhysicsStateConstructionCollider> rows) noexcept {
            for (std::size_t i = 0; i < rows.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    if (rows[i].collider == rows[j].collider)
                        return false;
            return true;
        }

        bool InitialVelocityOmitted(const PhysicsAuthoredBodyDescriptor &p) noexcept {
            return p.initialLinearVelocity.x == 0 && p.initialLinearVelocity.y == 0 && p.initialLinearVelocity.z == 0 &&
                   p.initialAngularVelocity.x == 0 && p.initialAngularVelocity.y == 0 && p.initialAngularVelocity.z == 0;
        }

        bool MassSubprofileValid(const PhysicsStateConstructionBody &body) noexcept {
            const auto original = body.floatingPoint;
            const auto guarded = body.massArithmeticSubprofile;
            if (guarded.architecture != original.architecture)
                return false;
            if (guarded == original)
                return true;
            if (original.architecture == PhysicsStateFpArchitecture::X86MxcsrX87)
                return guarded.value == (original.value | std::uint64_t{0x80});
            if (original.architecture == PhysicsStateFpArchitecture::ArmFpcr)
                return guarded.value == (original.value & ~(std::uint64_t{1} << 8));
            return false;
        }

        Result<void> BodyScalars(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionBody &b) {
            if (!HasShape(m, b.shape) || !InertiaValid(b) || !MotionStorageValid(b) || !InitialVelocityOmitted(b.policy) ||
                !MassSubprofileValid(b))
                return MalformedConstruction();
            if (auto pose = ValidatePhysicsPose(b.originalPose); pose.HasError())
                return pose;
            return ValidatePhysicsAuthoredBodyDescriptor(b.policy);
        }

        bool ColliderSlice(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionBody &b,
                           std::size_t nextCollider) noexcept {
            if (b.colliderBegin != nextCollider || b.colliderBegin > m.colliders.size() ||
                b.colliderCount > m.colliders.size() - b.colliderBegin)
                return false;
            if (b.profile && !b.profile->IsValid())
                return false;
            return b.profile.has_value() || b.colliderCount == 0;
        }

        Result<void> Body(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionBody &b, std::size_t &nextCollider,
                          std::size_t &nextMaterial) {
            if (auto scalars = BodyScalars(m, b); scalars.HasError())
                return scalars;
            if (!ColliderSlice(m, b, nextCollider))
                return MalformedConstruction();
            auto colliders = m.colliders.subspan(b.colliderBegin, b.colliderCount);
            if (!UniqueColliders(colliders))
                return MalformedConstruction();
            for (const auto &c : colliders)
                if (auto valid = Collider(m, c, nextMaterial); valid.HasError())
                    return valid;
            nextCollider += b.colliderCount;
            return Result<void>::Success();
        }

    }  // namespace

    Result<void> ValidateConstructionBodies(const PhysicsStateConstructionManifest &m, PhysicsStateFpControl current) {
        if (!Ordered(m.bodies, &PhysicsStateConstructionBody::id))
            return MalformedConstruction();
        for (const auto &material : m.materials)
            if (!ConstructionMaterialValid(material))
                return MalformedConstruction();
        std::size_t nextCollider{}, nextMaterial{};
        for (const auto &body : m.bodies) {
            if (body.floatingPoint != current)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            if (auto valid = Body(m, body, nextCollider, nextMaterial); valid.HasError())
                return valid;
        }
        return nextCollider == m.colliders.size() && nextMaterial == m.materials.size() ? Result<void>::Success() : MalformedConstruction();
    }
}  // namespace Horo::Physics::StateDetail
