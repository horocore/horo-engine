#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Sleep-test state is required even when the body currently sleeps. */
        bool ValidSleep(const PhysicsStateMotion &motion) noexcept {
            if (!std::isfinite(motion.sleepTimer) || motion.sleepTimer < 0)
                return false;
            return std::ranges::all_of(motion.sleepSpheres, [](const auto &sphere) {
                return Finite(sphere.center) && std::isfinite(sphere.radius) && sphere.radius >= 0;
            });
        }

        /** @brief All force and velocity channels are solver state, not reconstructed
         * from authored intent. */
        bool ValidMotion(const PhysicsStateMotion &motion) noexcept {
            return Finite(motion.linearVelocity) && Finite(motion.angularVelocity) && Finite(motion.force) && Finite(motion.torque) &&
                   ValidSleep(motion);
        }

        /** @brief Unused static-body channels cannot smuggle future required fields
         * into schema one. */
        bool EmptyMotion(const PhysicsStateMotion &motion) noexcept {
            return motion == PhysicsStateMotion{};
        }

        /** @brief Last-applied Horo intent is distinct from actual solver velocity and
         * absent on query fixtures. */
        bool ValidIntent(const PhysicsStateBody &body) noexcept {
            if (!body.hasPolicyIntent)
                return Zero(body.lastLinearIntent) && Zero(body.lastAngularIntent);
            return Finite(body.lastLinearIntent) && Finite(body.lastAngularIntent);
        }

        /** @brief Family-independent finite validation happens before any
         * family-specific comparison. */
        bool FiniteConstraint(const PhysicsStateConstraint &value) noexcept {
            const std::array scalars{value.axisLambda, value.motorLambda, value.limitLambda, value.targetVelocity, value.targetPosition};
            return Finite(value.rotationLambda) && Finite(value.pointLambda) && Finite(value.previousNormal) &&
                   Finite(value.transverseLambda) && Finite(scalars);
        }

        /** @brief Builds the exact supported field set without changing the submitted
         * candidate. */
        PhysicsStateConstraint UsedFields(const PhysicsStateConstraint &value) noexcept {
            PhysicsStateConstraint expected{.id = value.id, .kind = value.kind, .enabled = value.enabled};
            switch (value.kind) {
                using enum Horo::Physics::PhysicsStateConstraintKind;
                case Fixed:
                    expected.rotationLambda = value.rotationLambda;
                    expected.pointLambda = value.pointLambda;
                    break;
                case Distance:
                    expected.axisLambda = value.axisLambda;
                    expected.previousNormal = value.previousNormal;
                    break;
                case Hinge:
                    expected.pointLambda = value.pointLambda;
                    break;
                case Slider:
                    expected.rotationLambda = value.rotationLambda;
                    break;
            }
            return expected;
        }

        /** @brief Hinge/slider motor fields are retained even though their default Horo
         * policy disables motors. */
        bool ValidConstraint(const PhysicsStateConstraint &value) noexcept {
            using enum Horo::Physics::PhysicsStateConstraintKind;
            if (value.kind > Slider || !FiniteConstraint(value))
                return false;
            auto expected = UsedFields(value);
            if (value.kind == Hinge || value.kind == Slider) {
                expected.transverseLambda = value.transverseLambda;
                expected.motorLambda = value.motorLambda;
                expected.limitLambda = value.limitLambda;
                expected.motorState = value.motorState;
                expected.targetVelocity = value.targetVelocity;
                expected.targetPosition = value.targetPosition;
            }
            return value.motorState <= 3 && expected == value;
        }
    }  // namespace

    /** @copydoc ValidateBodies */
    Result<void> ValidateBodies(const std::span<const PhysicsStateBody> bodies) {
        PhysicsStateObjectId previous{};
        for (const auto &body : bodies) {
            if (body.id <= previous || !Finite(body.centerOfMass) || !UnitRotation(body.rotation) || !ValidIntent(body))
                return Invalid("Physics snapshot bodies require ordered stable IDs and "
                               "finite unit poses.");
            if (body.hasMotion ? !ValidMotion(body.motion) : body.active || !EmptyMotion(body.motion))
                return Invalid("Physics snapshot motion state disagrees with its "
                               "explicit body section.");
            previous = body.id;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateConstraints */
    Result<void> ValidateConstraints(const std::span<const PhysicsStateConstraint> constraints) {
        PhysicsStateObjectId previous{};
        for (const auto &constraint : constraints) {
            if (constraint.id <= previous || !ValidConstraint(constraint))
                return Invalid("Physics snapshot joint IDs, kind, warm-start fields or "
                               "motor state are invalid.");
            previous = constraint.id;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
