#include "PhysicsStateConstructionCommonFields.h"

namespace Horo::Physics::StateDetail {
    using namespace ConstructionFields;

    namespace {
        bool Mass(Wire &wire, PhysicsMassPolicy &v, bool reading) noexcept {
            auto kind = static_cast<std::uint8_t>(v.index());
            float value{};
            if (const auto *mass = std::get_if<PhysicsMass>(&v))
                value = mass->kilograms;
            if (const auto *density = std::get_if<PhysicsDensity>(&v))
                value = density->kilogramsPerCubicMeter;
            wire.Integer(kind);
            wire.Float(value);
            if (kind > 2)
                return false;
            if (reading) {
                switch (kind) {
                    case 0:
                        v = PhysicsNoMass{};
                        break;
                    case 1:
                        v = PhysicsMass{value};
                        break;
                    case 2:
                        v = PhysicsDensity{value};
                        break;
                }
            }
            return true;
        }

        void Safety(Wire &wire, PhysicsMotionSafety &v) noexcept {
            wire.Float(v.linearDampingPerSecond);
            wire.Float(v.angularDampingPerSecond);
            Enum(wire, v.lockedAxes);
            wire.Float(v.maximumLinearSpeed);
            wire.Float(v.maximumAngularSpeed);
            wire.Float(v.maximumDepenetrationSpeed);
        }

        bool Policy(Wire &wire, PhysicsAuthoredBodyDescriptor &v, bool reading) noexcept {
            Enum(wire, v.motion);
            if (!Mass(wire, v.mass, reading))
                return false;
            if (!reading) {
                v.initialLinearVelocity = {};
                v.initialAngularVelocity = {};
            }
            Vector(wire, v.initialLinearVelocity);
            Vector(wire, v.initialAngularVelocity);
            Safety(wire, v.motionSafety);
            Optional(wire, v.continuousCollision.mode, [](Wire &w, PhysicsDefaultMotionQuality &mode) {
                Enum(w, mode);
            });
            return true;
        }
    }  // namespace

    void ConstructionManifestHeaderFields(Wire &wire, ConstructionManifestHeader &v) noexcept {
        wire.Integer(v.tag);
        wire.Integer(v.shapeBytes);
        for (auto &count : v.counts)
            wire.Integer(count);
    }

    bool ConstructionBodyFields(Wire &wire, PhysicsStateConstructionBody &v, bool reading) noexcept {
        wire.Integer(v.id);
        wire.Integer(v.shape);
        Pose(wire, v.originalPose);
        if (!Policy(wire, v.policy, reading))
            return false;
        Optional(wire, v.profile, [](Wire &w, CollisionProfileId &id) {
            ProjectId(w, id);
        });
        wire.Integer(v.colliderBegin);
        wire.Integer(v.colliderCount);
        wire.Boolean(v.sensor);
        wire.Boolean(v.motionStorageReserved);
        wire.Boolean(v.hasMotion);
        wire.Float(v.inverseMass);
        wire.Floats(v.inverseInertia);
        wire.Floats(v.inertiaRotation);
        Fp(wire, v.floatingPoint);
        Fp(wire, v.massArithmeticSubprofile);
        return true;
    }

    void ConstructionMaterialFields(Wire &wire, PhysicsQueryMaterial &v) noexcept {
        ProjectId(wire, v.asset);
        wire.Integer(v.assetGeneration);
        auto slot = v.slot.Value();
        wire.Integer(slot);
        v.slot = PhysicsMaterialSlotId::FromValue(slot);
    }

    void ConstructionColliderFields(Wire &wire, PhysicsStateConstructionCollider &v) noexcept {
        Pose(wire, v.localPose);
        Optional(wire, v.subshape, Subshape);
        wire.Integer(v.materialBegin);
        wire.Integer(v.materialCount);
        Optional(wire, v.surface, Surface);
        wire.Integer(v.collider);
    }
}  // namespace Horo::Physics::StateDetail
