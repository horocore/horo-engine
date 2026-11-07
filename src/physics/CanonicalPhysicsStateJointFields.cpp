#include "CanonicalPhysicsStateFields.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Fixed native state retains both complete three-axis warm-start
         * impulses. */
        void FixedFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint) {
            fields.Vector(joint.rotationLambda);
            fields.Vector(joint.pointLambda);
        }

        /** @brief Distance zero-separation normal is hidden state and cannot be
         * recomputed from coincident endpoints. */
        void DistanceFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint) {
            fields.Value(joint.axisLambda);
            fields.Vector(joint.previousNormal);
        }

        /** @brief Actual hinge order is motor, two-axis rotation, point, limit, motor
         * policy/targets. */
        void HingeFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint) {
            fields.Value(joint.motorLambda);
            fields.Lambda2(joint.transverseLambda);
            fields.Vector(joint.pointLambda);
            fields.Value(joint.limitLambda);
            fields.Motor(joint.motorState);
            fields.Value(joint.targetVelocity);
            fields.Value(joint.targetPosition);
        }

        /** @brief Actual slider order is motor, two-axis position, quaternion rotation,
         * limit, motor policy/targets. */
        void SliderFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint) {
            fields.Value(joint.motorLambda);
            fields.Lambda2(joint.transverseLambda);
            fields.Vector(joint.rotationLambda);
            fields.Value(joint.limitLambda);
            fields.Motor(joint.motorState);
            fields.Value(joint.targetVelocity);
            fields.Value(joint.targetPosition);
        }
    }  // namespace

    /** @brief Actual supported Constraint::SaveState family grammars; caller has
     * validated exact retained family first. */
    void CanonicalJointFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint) {
        fields.Value(joint.enabled);
        switch (joint.kind) {
            case PhysicsStateConstraintKind::Fixed:
                FixedFields(fields, joint);
                break;
            case PhysicsStateConstraintKind::Distance:
                DistanceFields(fields, joint);
                break;
            case PhysicsStateConstraintKind::Hinge:
                HingeFields(fields, joint);
                break;
            case PhysicsStateConstraintKind::Slider:
                SliderFields(fields, joint);
                break;
        }
    }
}  // namespace Horo::Physics::Detail
