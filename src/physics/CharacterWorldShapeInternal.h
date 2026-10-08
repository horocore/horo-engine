#pragma once

#include "CharacterWorldPlacementInternal.h"

namespace Horo::Character::Detail {
    /** @brief Exact candidate geometry and its intended committed stance. */
    struct ShapeChangeTarget final {
        Physics::PhysicsCapsuleShape capsule;
        CharacterStance stance;
    };

    /** @brief Checks exact capsule dimensions without a geometric tolerance. */
    [[nodiscard]] bool SameCapsule(const Physics::PhysicsCapsuleShape &left, const Physics::PhysicsCapsuleShape &right) noexcept {
        return left.radiusMeters == right.radiusMeters && left.cylindricalHalfHeightMeters == right.cylindricalHalfHeightMeters;
    }

    /** @brief Selects an explicit or named target; conflicting intent and absent profiles have no candidate. */
    [[nodiscard]] std::optional<ShapeChangeTarget> SelectShapeChangeTarget(const CharacterMovementRequest &command,
                                                                           const CharacterControllerDescriptor &authored) noexcept {
        using enum CharacterStanceIntent;
        using enum CharacterStance;
        if (command.shapeChange.has_value()) {
            if (command.stance != Keep)
                return std::nullopt;
            return ShapeChangeTarget{command.shapeChange->capsule, Custom};
        }
        if (command.stance == Stand)
            return ShapeChangeTarget{authored.capsule, Standing};
        if (command.stance == Crouch && authored.crouchedCapsule.has_value())
            return ShapeChangeTarget{*authored.crouchedCapsule, Crouched};
        return std::nullopt;
    }

    /** @brief Admits positive finite capsule dimensions within the qualified local geometry envelope. */
    [[nodiscard]] bool IsShapeChangeGeometrySupported(const Physics::PhysicsCapsuleShape capsule) noexcept {
        return std::isfinite(capsule.radiusMeters) && capsule.radiusMeters > 0 && std::isfinite(capsule.cylindricalHalfHeightMeters) &&
               capsule.cylindricalHalfHeightMeters > 0 &&
               static_cast<double>(capsule.radiusMeters) + capsule.cylindricalHalfHeightMeters <=
                   Physics::MaximumPhysicsLocalHalfExtentMeters;
    }

    /** @brief Shifts the center along the owned up axis to retain the capsule bottom. */
    [[nodiscard]] Math::Vec3 BottomPreservingPosition(const Math::Vec3 position, const Math::Vec3 up,
                                                      const Physics::PhysicsCapsuleShape current,
                                                      const Physics::PhysicsCapsuleShape target) noexcept {
        const float currentExtent = current.radiusMeters + current.cylindricalHalfHeightMeters;
        const float targetExtent = target.radiusMeters + target.cylindricalHalfHeightMeters;
        return position + up * (targetExtent - currentExtent);
    }

    /** @brief Probes candidate clearance once, preserving original query errors and stopping on shutdown. */
    [[nodiscard]] Result<bool> ProbeShapeClearance(auto &impl, const CharacterMovementRequest &command,
                                                   const CharacterFixedTickInput &input, const CharacterControllerDescriptor &authored,
                                                   const Physics::PhysicsCapsuleShape target, const Math::Vec3 position) {
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return Result<bool>::Failure(budget.ErrorValue());
        if (const auto valid = ValidateQueryContext(impl, input.query, input.tick); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        const CharacterOverlapProbeRequest probeRequest{command.controller,
                                                        authored.sceneGeneration,
                                                        authored.characterWorld,
                                                        authored.physicsWorld,
                                                        target,
                                                        position,
                                                        authored.up,
                                                        authored.collisionProfile,
                                                        authored.queryChannel,
                                                        0,
                                                        authored.selectors};
        if (input.metrics != nullptr)
            ++input.metrics->snapshot.queries;
        const auto probe = input.query.overlap(input.query.context, probeRequest);
        if (const auto continuation = ValidateTickQueryContinuation(impl, probe); continuation.HasError())
            return Result<bool>::Failure(continuation.ErrorValue());
        if (!Math::IsFinite(probe.Value().recoveryDisplacement))
            return Result<bool>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        // Clearance consumes blocking presence only; a blocked candidate needs no recovery vector.
        if (probe.Value().overlapCount == 0) {
            if (const auto valid = ValidateCharacterOverlapProbeResult(probe.Value()); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
        }
        return Result<bool>::Success(probe.Value().overlapCount == 0);
    }

    /** @brief Resolves a supported instantaneous shape command without changing any live record. */
    [[nodiscard]] Result<CharacterShapeChangeResult> ResolveShapeChange(auto &impl, const CharacterMovementRequest &command,
                                                                        const CharacterFixedTickInput &input,
                                                                        const CharacterTransformPublication &previous,
                                                                        const CharacterControllerDescriptor &authored,
                                                                        const Physics::PhysicsCapsuleShape current,
                                                                        const CharacterStance stance) {
        using enum CharacterShapeChangeStatus;
        CharacterShapeChangeResult result{Invalid, current, stance};
        const auto target = SelectShapeChangeTarget(command, authored);
        if (!target.has_value() || !IsShapeChangeGeometrySupported(target->capsule))
            return Result<CharacterShapeChangeResult>::Success(result);
        if (!SameCapsule(current, target->capsule)) {
            const auto position = BottomPreservingPosition(previous.position, authored.up, current, target->capsule);
            if (const auto valid = ValidatePlacementDisplacement(previous.position, position,
                                                                 impl.settings.Values().work.maximumDisplacementMetersPerTick);
                valid.HasError())
                return Result<CharacterShapeChangeResult>::Failure(valid.ErrorValue());
            const auto clear = ProbeShapeClearance(impl, command, input, authored, target->capsule, position);
            if (clear.HasError())
                return Result<CharacterShapeChangeResult>::Failure(clear.ErrorValue());
            if (!clear.Value()) {
                result.status = Blocked;
                return Result<CharacterShapeChangeResult>::Success(result);
            }
        }
        return Result<CharacterShapeChangeResult>::Success({Applied, target->capsule, target->stance});
    }
}  // namespace Horo::Character::Detail
