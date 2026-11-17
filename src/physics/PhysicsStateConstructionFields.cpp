#include "PhysicsStateConstructionCodec.h"

namespace Horo::Physics::StateDetail {
    namespace {
        void Vector(Wire &wire, Math::Vec3 &v) noexcept {
            wire.Float(v.x);
            wire.Float(v.y);
            wire.Float(v.z);
        }

        void Pose(Wire &wire, PhysicsPose &v) noexcept {
            Vector(wire, v.translation);
            wire.Float(v.rotation.x);
            wire.Float(v.rotation.y);
            wire.Float(v.rotation.z);
            wire.Float(v.rotation.w);
        }

        void Shape(Wire &wire, PhysicsBoxShape &v) noexcept {
            Vector(wire, v.halfExtentsMeters);
        }

        void Shape(Wire &wire, PhysicsSphereShape &v) noexcept {
            wire.Float(v.radiusMeters);
        }

        void Shape(Wire &wire, PhysicsCapsuleShape &v) noexcept {
            wire.Float(v.radiusMeters);
            wire.Float(v.cylindricalHalfHeightMeters);
        }

        void Shape(Wire &wire, PhysicsStaticPlaneShape &v) noexcept {
            Vector(wire, v.normal);
            wire.Float(v.signedDistanceMeters);
        }

        bool SelectAnalytic(std::uint8_t kind, PhysicsShapeDescriptor &v) noexcept {
            switch (kind) {
                case 0:
                    v = PhysicsBoxShape{};
                    return true;
                case 1:
                    v = PhysicsSphereShape{};
                    return true;
                case 2:
                    v = PhysicsCapsuleShape{};
                    return true;
                case 3:
                    v = PhysicsStaticPlaneShape{};
                    return true;
                default:
                    return false;
            }
        }

        bool Analytic(Wire &wire, PhysicsShapeDescriptor &v, bool reading) noexcept {
            auto kind = static_cast<std::uint8_t>(v.index());
            wire.Integer(kind);
            if (reading && !SelectAnalytic(kind, v))
                return false;
            std::visit([&](auto &shape) {
                Shape(wire, shape);
            }, v);
            return true;
        }
    }  // namespace

    void ConstructionShapeHeaderFields(Wire &wire, ConstructionShapeHeader &v) noexcept {
        wire.Integer(v.tag);
        wire.Integer(v.nodes);
        wire.Integer(v.edges);
        wire.Integer(v.roots);
    }

    bool ConstructionShapeNodeFields(Wire &wire, PhysicsStateShapeRecipe &v, bool reading) noexcept {
        auto arch = static_cast<std::uint8_t>(v.floatingPoint.architecture);
        wire.Integer(arch);
        wire.Integer(v.floatingPoint.value);
        if (reading)
            v.floatingPoint.architecture = static_cast<PhysicsStateFpArchitecture>(arch);
        auto kind = static_cast<std::uint8_t>(v.geometry.index());
        wire.Integer(kind);
        if (kind > 1)
            return false;
        if (kind == 0) {
            if (reading)
                v.geometry = PhysicsShapeDescriptor{};
            return Analytic(wire, std::get<PhysicsShapeDescriptor>(v.geometry), reading);
        }
        if (reading)
            v.geometry = PhysicsStateCompoundRecipe{};
        auto &group = std::get<PhysicsStateCompoundRecipe>(v.geometry);
        wire.Integer(group.begin);
        wire.Integer(group.count);
        return true;
    }

    void ConstructionShapeEdgeFields(Wire &wire, PhysicsStateShapeRecipeEdge &v) noexcept {
        wire.Integer(v.child);
        Pose(wire, v.localPose);
        wire.Integer(v.userData);
    }

    void ConstructionShapeRootFields(Wire &wire, PhysicsStateShapeRecipeRoot &v) noexcept {
        wire.Integer(v.shape);
        wire.Integer(v.recipe);
    }
}  // namespace Horo::Physics::StateDetail
