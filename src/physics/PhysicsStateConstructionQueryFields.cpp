#include "PhysicsStateConstructionCommonFields.h"

namespace Horo::Physics::StateDetail {
    using namespace ConstructionFields;

    namespace {
        template <class T> void Selectors(Wire &wire, T &v) noexcept {
            ProjectId(wire, v.layer);
            ProjectId(wire, v.profile);
            ProjectId(wire, v.channel);
            Enum(wire, v.response);
            wire.Boolean(v.trigger);
        }
    }  // namespace

    void ConstructionFixtureFields(Wire &wire, PhysicsStateConstructionFixture &v) noexcept {
        wire.Integer(v.body);
        wire.Integer(v.shape);
        Pose(wire, v.originalPose);
        Selectors(wire, v);
        Optional(wire, v.subshape, Subshape);
        Optional(wire, v.material, ConstructionMaterialFields);
        Optional(wire, v.surface, Surface);
        wire.Integer(v.childBegin);
        wire.Integer(v.childCount);
        Fp(wire, v.floatingPoint);
    }

    void ConstructionQueryChildFields(Wire &wire, PhysicsStateConstructionQueryChild &v) noexcept {
        Subshape(wire, v.subshape);
        Optional(wire, v.material, ConstructionMaterialFields);
        Selectors(wire, v);
        Optional(wire, v.surface, Surface);
    }
}  // namespace Horo::Physics::StateDetail
