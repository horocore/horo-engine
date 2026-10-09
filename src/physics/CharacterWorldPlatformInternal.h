#pragma once

#include "CharacterWorldInternal.h"

namespace Horo::Character::Detail {
    /** @brief Compares the complete non-owning movement-base binding, never just the body slot. */
    [[nodiscard]] bool SamePlatformBase(const CharacterPlatformAttachment &previous, const CharacterMovementResult &movement) noexcept {
        return movement.groundBody == previous.body && movement.groundShape == previous.shape &&
               movement.groundSubshape == previous.subshape;
    }

    /** @brief Rejects malformed or mismatched copied body evidence before local-frame arithmetic. */
    [[nodiscard]] Result<void> ValidatePlatformBodyEvidence(const CharacterPlatformBodyEvidence &evidence,
                                                            const CharacterMovementResult &movement) {
        using enum Physics::PhysicsMotionType;
        if (movement.groundBody != evidence.body || movement.groundShape != evidence.shape)
            return Result<void>::Failure(MakeError(CharacterErrors::RequestInvalid));
        if (evidence.motion != Static && evidence.motion != Kinematic && evidence.motion != Dynamic)
            return Result<void>::Failure(MakeError(CharacterErrors::RequestInvalid));
        return Physics::ValidatePhysicsPose(evidence.pose);
    }

    /** @brief Converts a committed candidate's support and root into the exact sampled body-local frame. */
    [[nodiscard]] CharacterPlatformAttachment LocalPlatformFrame(const CharacterMovementResult &movement,
                                                                 const CharacterPlatformBodyEvidence &evidence,
                                                                 const CharacterPhysicsQueryContext &query) noexcept {
        const auto inverse = evidence.pose.rotation.Inverse();
        return {*movement.groundBody,
                movement.groundShape,
                movement.groundSubshape,
                inverse.Rotate(movement.groundPoint - evidence.pose.translation),
                Math::Normalize(inverse.Rotate(movement.groundNormal)),
                {inverse.Rotate(movement.finalPosition - evidence.pose.translation), (inverse * movement.finalHeading).Normalized()},
                evidence.pose,
                query.tick,
                query.physicsSnapshotRevision,
                evidence.motion};
    }

    /** @brief Resolves exactly one bounded live-body read and preserves typed failure/lifecycle fencing. */
    [[nodiscard]] Result<std::optional<CharacterPlatformBodyEvidence>> ReadPlatformBody(auto &impl, const CharacterMovementResult &movement,
                                                                                        const CharacterFixedTickInput &input) {
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return Result<std::optional<CharacterPlatformBodyEvidence>>::Failure(budget.ErrorValue());
        if (input.metrics != nullptr)
            ++input.metrics->snapshot.queries;
        const auto evidence =
            input.query.platformBody(input.query.context, *movement.groundBody, movement.groundShape, input.query.physicsSnapshotRevision);
        if (const auto continuation = ValidateTickQueryContinuation(impl, evidence); continuation.HasError())
            return Result<std::optional<CharacterPlatformBodyEvidence>>::Failure(continuation.ErrorValue());
        return evidence;
    }

    /** @brief Classifies one complete attachment identity independently of its sampled frame. */
    [[nodiscard]] CharacterPlatformAttachmentChange PlatformBindingChange(const std::optional<CharacterPlatformAttachment> &previous,
                                                                          const CharacterMovementResult &movement) noexcept {
        using enum CharacterPlatformAttachmentChange;
        if (!previous)
            return Attached;
        return SamePlatformBase(*previous, movement) ? None : BaseChanged;
    }

    /** @brief Validates eligible evidence and stages its owned local frame without publishing controller state. */
    [[nodiscard]] Result<void> StagePlatformAttachment(CharacterMovementResult &movement, const CharacterPlatformBodyEvidence &evidence,
                                                       const CharacterFixedTickInput &input,
                                                       const CharacterControllerDescriptor &descriptor,
                                                       const std::optional<CharacterPlatformAttachment> &previous) {
        if (const auto valid = ValidatePlatformBodyEvidence(evidence, movement); valid.HasError())
            return valid;
        if (evidence.motion == Physics::PhysicsMotionType::Dynamic && !descriptor.allowDynamicPlatformAttachment)
            return Result<void>::Success();
        movement.platformAttachment = LocalPlatformFrame(movement, evidence, input.query);
        movement.platformAttached = true;
        movement.platformAttachmentChange = PlatformBindingChange(previous, movement);
        return Result<void>::Success();
    }

    /** @brief Stages attachment only from eligible final support; no live reference or mutation escapes this operation. */
    [[nodiscard]] Result<void> ResolvePlatformAttachment(auto &impl, CharacterMovementResult &movement,
                                                         const CharacterFixedTickInput &input,
                                                         const CharacterControllerDescriptor &descriptor,
                                                         const std::optional<CharacterPlatformAttachment> &previous) {
        using enum CharacterPlatformAttachmentChange;
        movement.platformAttachment.reset();
        movement.platformAttached = false;
        movement.platformAttachmentChange = previous ? Detached : None;
        if (!movement.grounded || !movement.groundBody || movement.jumpApplied)
            return Result<void>::Success();
        if (!input.query.platformBody) {
            movement.platformAttachmentChange = Unavailable;
            return Result<void>::Success();
        }
        const auto evidence = ReadPlatformBody(impl, movement, input);
        if (evidence.HasError())
            return Result<void>::Failure(evidence.ErrorValue());
        if (!evidence.Value()) {
            movement.platformAttachmentChange = previous && SamePlatformBase(*previous, movement) ? Stale : Unavailable;
            return Result<void>::Success();
        }
        return StagePlatformAttachment(movement, *evidence.Value(), input, descriptor, previous);
    }

    /** @brief Admits the exact query view and validates the staged support binding before publication preflight. */
    [[nodiscard]] Result<void> ResolveValidatedPlatformAttachment(auto &impl, CharacterMovementResult &movement,
                                                                  const CharacterFixedTickInput &input,
                                                                  const CharacterControllerDescriptor &descriptor,
                                                                  const std::optional<CharacterPlatformAttachment> &previous) {
        const CharacterPhysicsQueryExpectations expected{impl.descriptor.sceneGeneration,        impl.descriptor.identity,
                                                         impl.descriptor.physicsWorld,           impl.descriptor.collisionFilterGeneration,
                                                         impl.descriptor.originGeneration,       input.tick,
                                                         impl.descriptor.physicsSnapshotRevision};
        if (input.query.platformBody) {
            if (const auto valid = ValidateCharacterPhysicsQueryContext(input.query, expected); valid.HasError())
                return valid;
        }
        if (const auto attachment = ResolvePlatformAttachment(impl, movement, input, descriptor, previous); attachment.HasError())
            return attachment;
        return ValidateCharacterMovementResult(movement, descriptor);
    }
}  // namespace Horo::Character::Detail
