#pragma once
#include "PhysicsStateConstructionManifestCodec.h"
#include "PhysicsStateEventFields.h"

namespace Horo::Physics::StateDetail::ConstructionFields {
    template <class T> void Enum(Wire &wire, T &v) noexcept {
        auto bits = static_cast<std::uint8_t>(v);
        wire.Integer(bits);
        v = static_cast<T>(bits);
    }

    inline void Vector(Wire &wire, Math::Vec3 &v) noexcept {
        wire.Float(v.x);
        wire.Float(v.y);
        wire.Float(v.z);
    }

    inline void Pose(Wire &wire, PhysicsPose &v) noexcept {
        Vector(wire, v.translation);
        wire.Float(v.rotation.x);
        wire.Float(v.rotation.y);
        wire.Float(v.rotation.z);
        wire.Float(v.rotation.w);
    }

    inline void Fp(Wire &wire, PhysicsStateFpControl &v) noexcept {
        Enum(wire, v.architecture);
        wire.Integer(v.value);
    }

    inline void Subshape(Wire &wire, PhysicsShapeSubresourceId &v) noexcept {
        auto id = v.Value();
        wire.Integer(id);
        v = PhysicsShapeSubresourceId::FromValue(id);
    }

    inline void Com(Wire &wire, PhysicsStateConstructionCom &v) noexcept {
        wire.Floats(v.position);
        wire.Floats(v.rotation);
    }
}  // namespace Horo::Physics::StateDetail::ConstructionFields
