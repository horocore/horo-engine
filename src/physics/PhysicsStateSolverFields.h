#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"
#include "PhysicsStateWireInternal.h"

namespace Horo::Physics::StateDetail {
    /** @brief Every moving-body scalar has a fixed required wire location,
     * including dormant sleep state. */
    inline void MotionFields(Wire &wire, PhysicsStateMotion &value) noexcept {
        wire.Floats(value.linearVelocity);
        wire.Floats(value.angularVelocity);
        wire.Floats(value.force);
        wire.Floats(value.torque);
        for (auto &sphere : value.sleepSpheres) {
            wire.Floats(sphere.center);
            wire.Float(sphere.radius);
        }
        wire.Float(value.sleepTimer);
        wire.Boolean(value.allowSleeping);
    }

    /** @brief Body section uses stable identity and explicit motion presence;
     * native ID/padding is excluded. */
    inline void BodyFields(Wire &wire, PhysicsStateBody &value) noexcept {
        wire.Integer(value.id);
        wire.Floats(value.centerOfMass);
        wire.Floats(value.rotation);
        wire.Boolean(value.hasMotion);
        wire.Boolean(value.active);
        MotionFields(wire, value.motion);
        wire.Boolean(value.hasPolicyIntent);
        wire.Floats(value.lastLinearIntent);
        wire.Floats(value.lastAngularIntent);
    }

    /** @brief All joint fields are required even when zero, so unknown additions
     * cannot be ignored. */
    inline void ConstraintFields(Wire &wire, PhysicsStateConstraint &value) noexcept {
        wire.Integer(value.id);
        auto kind = static_cast<std::uint8_t>(value.kind);
        wire.Integer(kind);
        value.kind = static_cast<PhysicsStateConstraintKind>(kind);
        wire.Boolean(value.enabled);
        wire.Floats(value.rotationLambda);
        wire.Floats(value.pointLambda);
        wire.Floats(value.previousNormal);
        wire.Floats(value.transverseLambda);
        wire.Float(value.axisLambda);
        wire.Float(value.motorLambda);
        wire.Float(value.limitLambda);
        wire.Float(value.targetVelocity);
        wire.Float(value.targetPosition);
        wire.Integer(value.motorState);
    }

    /** @brief Stable pair endpoint orientation is semantic and never silently
     * reversed. */
    inline void ManifoldKeyFields(Wire &wire, PhysicsStateManifoldKey &value) noexcept {
        wire.Integer(value.first);
        wire.Integer(value.second);
        wire.Integer(value.firstSubshape);
        wire.Integer(value.secondSubshape);
    }

    /** @brief Pair metadata refers to an already checked contiguous typed manifold
     * range. */
    inline void PairFields(Wire &wire, PhysicsStateBodyPair &value) noexcept {
        wire.Integer(value.first);
        wire.Integer(value.second);
        wire.Floats(value.relativePosition);
        wire.Floats(value.relativeRotation);
        wire.Integer(value.manifoldBegin);
        wire.Integer(value.manifoldCount);
    }

    /** @brief Encodes every warm-start contact point rather than gameplay's
     * potentially truncated summary. */
    inline void ManifoldFields(Wire &wire, PhysicsStateManifold &value) noexcept {
        ManifoldKeyFields(wire, value.key);
        wire.Floats(value.normal);
        wire.Floats(value.frictionLambda);
        wire.Float(value.angularFrictionLambda);
        wire.Integer(value.pointCount);
        for (auto &point : value.points) {
            wire.Floats(point.position1);
            wire.Floats(point.position2);
            wire.Float(point.normalLambda);
        }
    }
}  // namespace Horo::Physics::StateDetail
