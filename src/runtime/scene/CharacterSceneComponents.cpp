#include "Horo/Runtime/Scene/CharacterSceneComponents.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>

namespace Horo::Runtime {
    namespace {
        /** @brief Checks finite positive capsule dimensions, including overflow of the total extent. */
        [[nodiscard]] bool ValidCapsule(const PhysicsCapsuleCollider &shape) noexcept {
            return std::isfinite(shape.radiusMeters) && shape.radiusMeters > 0 && std::isfinite(shape.cylindricalHalfHeightMeters) &&
                   shape.cylindricalHalfHeightMeters > 0 && std::isfinite(shape.radiusMeters + shape.cylindricalHalfHeightMeters);
        }

        /** @brief Checks the same unit-basis tolerance used by the resident Character descriptor. */
        [[nodiscard]] bool ValidGeometry(const CharacterSceneGeometry &value) noexcept {
            if (!ValidCapsule(value.capsule) || !Math::IsFinite(value.up) || !Math::IsFinite(value.gravity) ||
                std::abs(static_cast<double>(Math::Dot(value.up, value.up)) - 1.0) > Character::CharacterUnitSquaredNormTolerance)
                return false;
            return !value.crouchedCapsule ||
                   (ValidCapsule(*value.crouchedCapsule) && value.crouchedCapsule->radiusMeters == value.capsule.radiusMeters &&
                    value.crouchedCapsule->cylindricalHalfHeightMeters < value.capsule.cylindricalHalfHeightMeters);
        }

        /** @brief Checks bounded policies even for a disabled component, avoiding deferred invalid authoring. */
        [[nodiscard]] bool ValidMovement(const CharacterSceneMovement &value) noexcept {
            const auto nonnegative = [](float scalar) {
                return std::isfinite(scalar) && scalar >= 0;
            };
            return std::isfinite(value.skinWidthMeters) && value.skinWidthMeters > 0 && nonnegative(value.minimumMoveDistanceMeters) &&
                   nonnegative(value.maximumStepHeightMeters) && nonnegative(value.maximumSlopeDegrees) &&
                   value.maximumSlopeDegrees <= 90 && nonnegative(value.jumpSpeedMetersPerSecond) && value.maximumContacts > 0 &&
                   value.maximumContacts <= Character::MaximumCharacterContacts &&
                   (value.steepSlopePolicy == Character::CharacterSteepSlopePolicy::Stop ||
                    value.steepSlopePolicy == Character::CharacterSteepSlopePolicy::Slide) &&
                   value.platformLeaveVelocity <= Character::CharacterPlatformLeaveVelocityPolicy::Point;
        }

        /** @brief Checks present identities without inventing defaults or resolving current project authority. */
        [[nodiscard]] bool ValidCollision(const CharacterSceneCollision &value, bool enabled) noexcept {
            return (!enabled || (value.profile && value.queryChannel && value.defaultMaterial)) &&
                   (!value.profile || value.profile->IsValid()) && (!value.queryChannel || value.queryChannel->IsValid()) &&
                   (!value.defaultMaterial || (value.defaultMaterial->material.IsValid() && value.defaultMaterial->slot.IsValid())) &&
                   (!value.requiredLayer || value.requiredLayer->IsValid()) &&
                   (!value.requiredProfile || value.requiredProfile->IsValid()) &&
                   (!value.excludedBody || (value.excludedBody->object.IsValid() && value.excludedBody->body.IsValid()));
        }
    }  // namespace

    /** @copydoc ValidateCharacterControllerComponent */
    Result<void> ValidateCharacterControllerComponent(const CharacterControllerComponent &component) {
        if (!component.id.IsValid() || component.schemaVersion != CharacterSceneComponentSchemaVersion ||
            !ValidGeometry(component.geometry) || !ValidMovement(component.movement) ||
            !ValidCollision(component.collision, component.enabled))
            return Result<void>::Failure(
                MakeError(Physics::PhysicsErrors::DescriptorInvalid, "Character component geometry, policy, or binding is invalid."));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
