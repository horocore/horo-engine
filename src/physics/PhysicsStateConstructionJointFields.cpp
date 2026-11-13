#include "PhysicsStateConstructionCommonFields.h"

namespace Horo::Physics::StateDetail {
    using namespace ConstructionFields;

    namespace {
        void Bounds(const PhysicsFixedConstraint &, float &, float &) noexcept {}

        void Bounds(const PhysicsDistanceConstraint &v, float &a, float &b) noexcept {
            a = v.minimumMeters;
            b = v.maximumMeters;
        }

        void Bounds(const PhysicsHingeConstraint &v, float &a, float &b) noexcept {
            a = v.minimumRadians;
            b = v.maximumRadians;
        }

        void Bounds(const PhysicsSliderConstraint &v, float &a, float &b) noexcept {
            a = v.minimumMeters;
            b = v.maximumMeters;
        }

        bool Parameters(Wire &wire, decltype(PhysicsConstraintDescriptor::parameters) &v, bool reading) noexcept {
            auto kind = static_cast<std::uint8_t>(v.index());
            float first{};
            float second{};
            std::visit([&](const auto &value) {
                Bounds(value, first, second);
            }, v);
            wire.Integer(kind);
            wire.Float(first);
            wire.Float(second);
            if (kind > 3)
                return false;
            if (reading) {
                switch (kind) {
                    case 0:
                        v = PhysicsFixedConstraint{};
                        break;
                    case 1:
                        v = PhysicsDistanceConstraint{first, second};
                        break;
                    case 2:
                        v = PhysicsHingeConstraint{first, second};
                        break;
                    case 3:
                        v = PhysicsSliderConstraint{first, second};
                        break;
                }
            }
            return true;
        }
    }  // namespace

    bool ConstructionJointFields(Wire &wire, PhysicsStateConstructionJoint &v, bool reading) noexcept {
        wire.Integer(v.id);
        wire.Integer(v.first);
        Optional(wire, v.second, [](Wire &w, PhysicsStateObjectId &id) {
            w.Integer(id);
        });
        Pose(wire, v.firstLocalFrame);
        Pose(wire, v.secondLocalOrWorldFrame);
        if (!Parameters(wire, v.parameters, reading))
            return false;
        Enum(wire, v.collisionPolicy);
        Pose(wire, v.firstConstructorFrame);
        Pose(wire, v.secondConstructorFrame);
        Com(wire, v.firstCom);
        Com(wire, v.secondCom);
        Fp(wire, v.floatingPoint);
        return true;
    }
}  // namespace Horo::Physics::StateDetail
