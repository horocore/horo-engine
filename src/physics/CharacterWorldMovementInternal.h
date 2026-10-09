#pragma once

/** @file
 * @brief Target-private fixed-tick movement orchestration and shared-budget continuation.
 * Only checked travel reaches the caller; failures still roll back the candidate tick.
 */
#include "CharacterWorldStepInternal.h"

#include <cmath>
#include <limits>

namespace Horo::Character::Detail {
    /** @brief Admits one synchronous sweep and returns validated, canonically ordered copied evidence. */
    [[nodiscard]] Result<CharacterSweepProbeResult> ReadMovementSweep(auto &impl, const CharacterMovementRequest &command,
                                                                      const CharacterFixedTickInput &input,
                                                                      const CharacterControllerDescriptor &descriptor,
                                                                      const SweepMotionState &motion, const Math::Vec3 direction,
                                                                      const float distance, const std::uint32_t iteration) {
        return ReadCapsuleSweep(impl, StepQueryContext{command, input, descriptor},
                                StepCast{motion.position, direction, distance, iteration}, SweepPurpose::Movement);
    }

    /** @brief Resolves one checked sweep, including step admission, without publishing a partial candidate. */
    [[nodiscard]] Result<bool> ResolveCapsuleSweepIteration(auto &impl, CharacterMovementResult &result,
                                                            const CharacterMovementRequest &command, const CharacterFixedTickInput &input,
                                                            const CharacterControllerDescriptor &descriptor, SweepMotionState &motion,
                                                            const std::uint32_t iteration) {
        const float distance = Math::Length(motion.remaining);
        if (!std::isfinite(distance) || distance <= descriptor.minimumMoveDistanceMeters)
            return Result<bool>::Success(false);
        const Math::Vec3 direction = motion.remaining / distance;
        auto probe = ReadMovementSweep(impl, command, input, descriptor, motion, direction, distance, iteration);
        if (probe.HasError())
            return Result<bool>::Failure(probe.ErrorValue());
        const auto &evidence = probe.Value();
        const auto selection = SelectNearestSweepBlock(evidence, direction, distance, descriptor);
        if (!selection.blocked) {
            motion.position += motion.remaining;
            motion.remaining = {};
            return Result<bool>::Success(false);
        }
        const bool lowObstacle = HasLowStepObstacle(evidence, descriptor, motion, direction, selection.nearest);
        if (lowObstacle) {
            const auto step = TryCapsuleStep(impl, result, command, input, descriptor, motion);
            if (step.HasError())
                return Result<bool>::Failure(step.ErrorValue());
            if (step.Value())
                return Result<bool>::Success(false);
        }
        const float seconds = static_cast<float>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0F;
        const float maximumAscent =
            std::max(0.0F, Math::Dot(command.desiredVelocityMetersPerSecond.value_or(Math::Vec3{}), descriptor.up)) * seconds +
            (motion.airbornePass ? std::max(0.0F, Math::Dot(motion.remaining, descriptor.up)) : 0.0F);
        ApplySweepBlock(result, evidence, descriptor, direction, selection.nearest, maximumAscent, motion);
        if (lowObstacle)
            ClampRejectedStepMotion(evidence, descriptor, selection.nearest, maximumAscent, motion);
        return Result<bool>::Success(true);
    }

    /** @brief Admits finite steep gravity travel or records a conservative shared-budget stop before issuing a cast. */
    [[nodiscard]] Result<bool> BeginSteepMotion(auto &impl, CharacterMovementResult &result,
                                                const CharacterControllerDescriptor &descriptor, const Math::Vec3 normal,
                                                SweepMotionState &motion) {
        const Math::Vec3 tangentGravity = descriptor.gravity - normal * Math::Dot(descriptor.gravity, normal);
        if (Math::Dot(tangentGravity, descriptor.up) >= 0.0F)
            return Result<bool>::Success(false);
        if (motion.iteration >= impl.settings.Values().work.maximumMovementIterations) {
            StopAtMovementLimit(result, motion);
            return Result<bool>::Success(false);
        }
        Math::Vec3 initialVelocity = motion.previousGravityVelocity - normal * Math::Dot(motion.previousGravityVelocity, normal);
        if (Math::Dot(initialVelocity, descriptor.up) > 0.0F)
            initialVelocity = {};
        motion.gravityVelocity = initialVelocity + tangentGravity * motion.elapsedSeconds;
        motion.remaining = (initialVelocity + motion.gravityVelocity) * (motion.elapsedSeconds * 0.5F);
        if (!Math::IsFinite(motion.remaining))
            return Result<bool>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        return Result<bool>::Success(true);
    }

    /** @brief Projects committed steep continuation without manufacturing ascent or changing the checked pose. */
    void PublishSteepContinuation(CharacterMovementResult &result, const CharacterControllerDescriptor &descriptor,
                                  const SweepMotionState &motion, const std::optional<CharacterSweepHit> &steepSupport) {
        result.gravityVelocityMetersPerSecond = result.grounded ? Math::Vec3{} : motion.gravityVelocity;
        if (steepSupport.has_value()) {
            const float walkableCosine = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F);
            const Math::Vec3 continuation = ClipSlopeMotion(motion.gravityVelocity, steepSupport->normal, descriptor, walkableCosine);
            if (Math::Dot(continuation, descriptor.up) <= GroundNormalTolerance)
                result.gravityVelocityMetersPerSecond = continuation;
        }
    }

    /** @brief Stages gravity continuation on one steep face within the remaining fixed-tick sweep budget. */
    [[nodiscard]] Result<void> ResolveSteepSliding(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                   const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                   SweepMotionState &motion, std::optional<CharacterSweepHit> &steepSupport) {
        if (!steepSupport.has_value() || descriptor.steepSlopePolicy != CharacterSteepSlopePolicy::Slide)
            return Result<void>::Success();
        const auto staged = BeginSteepMotion(impl, result, descriptor, steepSupport->normal, motion);
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        if (!staged.Value())
            return Result<void>::Success();
        auto slideDescriptor = descriptor;
        slideDescriptor.minimumMoveDistanceMeters = 0.0F;
        for (; motion.iteration < impl.settings.Values().work.maximumMovementIterations; ++motion.iteration) {
            const auto resolved = ResolveCapsuleSweepIteration(impl, result, command, input, slideDescriptor, motion, motion.iteration);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                break;
        }
        if (Math::LengthSquared(motion.remaining) > 0.0F)
            StopAtMovementLimit(result, motion);
        steepSupport.reset();
        if (const auto grounded = ResolveGrounding(impl, result, command, input, descriptor, motion.position, steepSupport);
            grounded.HasError())
            return Result<void>::Failure(grounded.ErrorValue());
        PublishSteepContinuation(result, descriptor, motion, steepSupport);
        return Result<void>::Success();
    }

    /** @brief Publishes only the free-flight continuation allowed by the final copied support evidence. */
    void PublishAirborneContinuation(CharacterMovementResult &result, const CharacterControllerDescriptor &descriptor,
                                     const SweepMotionState &motion, const std::optional<CharacterSweepHit> &steepSupport) {
        if (result.grounded)
            result.gravityVelocityMetersPerSecond = {};
        else if (steepSupport) {
            if (descriptor.steepSlopePolicy == CharacterSteepSlopePolicy::Stop)
                result.gravityVelocityMetersPerSecond = {};
            else
                result.gravityVelocityMetersPerSecond = ClipSlopeMotion(motion.gravityVelocity, steepSupport->normal, descriptor,
                                                                        std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F));
        }
    }

    /** @brief Integrates the single committed free-flight velocity through the remaining bounded sweeps. */
    [[nodiscard]] Result<void> ResolveAirborneMotion(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                     const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                     const CharacterControllerDescriptor &supportDescriptor, SweepMotionState &motion) {
        const Math::Vec3 initial = result.gravityVelocityMetersPerSecond;
        motion.gravityVelocity = initial + descriptor.gravity * motion.elapsedSeconds;
        motion.remaining = (initial + motion.gravityVelocity) * (motion.elapsedSeconds * 0.5F);
        motion.airbornePass = true;
        if (!Math::IsFinite(motion.remaining) || !Math::IsFinite(motion.gravityVelocity) ||
            Math::Length(motion.remaining) > impl.settings.Values().work.maximumDisplacementMetersPerTick)
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        auto airDescriptor = descriptor;
        // Acceleration below the ordinary intent threshold must not disappear each tick.
        airDescriptor.minimumMoveDistanceMeters = 0.0F;
        while (Math::LengthSquared(motion.remaining) > 0.0F) {
            if (motion.iteration >= impl.settings.Values().work.maximumMovementIterations) {
                StopAtMovementLimit(result, motion);
                break;
            }
            const auto swept = ResolveCapsuleSweepIteration(impl, result, command, input, airDescriptor, motion, motion.iteration++);
            if (swept.HasError())
                return Result<void>::Failure(swept.ErrorValue());
            if (!swept.Value())
                break;
        }
        result.gravityVelocityMetersPerSecond = motion.gravityVelocity;
        std::optional<CharacterSweepHit> steepSupport;
        // Free flight can acquire touching support but cannot restart step-height snapping.
        auto landingDescriptor = supportDescriptor;
        landingDescriptor.maximumStepHeightMeters = 0.0F;
        if (const auto support = ResolveGrounding(impl, result, command, input, landingDescriptor, motion.position, steepSupport);
            support.HasError())
            return support;
        PublishAirborneContinuation(result, descriptor, motion, steepSupport);
        return Result<void>::Success();
    }

    /** @brief Sweeps ordinary commanded displacement within the shared iteration budget before grounding and gravity. */
    [[nodiscard]] Result<void> ResolveDesiredDisplacement(auto &impl, CharacterMovementResult &result,
                                                          const CharacterMovementRequest &command, const CharacterFixedTickInput &input,
                                                          const CharacterControllerDescriptor &descriptor, SweepMotionState &motion) {
        for (; motion.iteration < impl.settings.Values().work.maximumMovementIterations; ++motion.iteration) {
            if (Math::LengthSquared(motion.remaining) <= descriptor.minimumMoveDistanceMeters * descriptor.minimumMoveDistanceMeters)
                return Result<void>::Success();
            const auto resolved = ResolveCapsuleSweepIteration(impl, result, command, input, descriptor, motion, motion.iteration);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            if (!resolved.Value()) {
                ++motion.iteration;
                break;
            }
        }
        if (Math::LengthSquared(motion.remaining) > descriptor.minimumMoveDistanceMeters * descriptor.minimumMoveDistanceMeters)
            StopAtMovementLimit(result, motion);
        return Result<void>::Success();
    }

    /** @brief Completes only admitted post-intent gravity phases; a solver stop cannot restart unswept motion. */
    [[nodiscard]] Result<void> ResolvePostSweepMotion(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                      const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                      const CharacterControllerDescriptor &supportDescriptor, SweepMotionState &motion,
                                                      std::optional<CharacterSweepHit> &steepSupport) {
        const bool steepPass = steepSupport.has_value();
        if (result.termination != CharacterMovementTermination::Complete) {
            motion.gravityVelocity = {};
            result.gravityVelocityMetersPerSecond = {};
            steepSupport.reset();
        }
        if (const auto slide = ResolveSteepSliding(impl, result, command, input, descriptor, motion, steepSupport); slide.HasError())
            return Result<void>::Failure(slide.ErrorValue());
        if (result.termination == CharacterMovementTermination::Complete && !result.grounded && !steepSupport.has_value() && !steepPass) {
            return ResolveAirborneMotion(impl, result, command, input, descriptor, supportDescriptor, motion);
        }
        if (result.grounded || descriptor.steepSlopePolicy == CharacterSteepSlopePolicy::Stop)
            result.gravityVelocityMetersPerSecond = {};
        return Result<void>::Success();
    }

    /** @brief Initializes the detached result correlation and committed jump continuation before movement begins. */
    [[nodiscard]] CharacterMovementResult InitialSweepMovement(const CharacterMovementRequest &command,
                                                               const CharacterTransformPublication &previous,
                                                               const CharacterControllerDescriptor &descriptor,
                                                               const Math::Vec3 previousGravityVelocity, const std::uint64_t tick) {
        CharacterMovementResult result;
        result.controller = command.controller;
        result.tick = tick;
        result.sequence = command.sequence;
        result.finalPosition = previous.position;
        result.finalHeading = command.desiredHeading.value_or(previous.heading);
        result.up = descriptor.up;
        ClearGroundEvidence(result, descriptor.up);
        result.jumpApplied = command.jumpRequested && previous.grounded && descriptor.jumpSpeedMetersPerSecond > 0.0F;
        result.gravityVelocityMetersPerSecond =
            result.jumpApplied ? descriptor.up * descriptor.jumpSpeedMetersPerSecond : previousGravityVelocity;
        return result;
    }

    /** @brief Admits finite requested travel before the first query, without creating publication state. */
    [[nodiscard]] Result<SweepMotionState> AdmitSweepMotion(const CharacterMovementRequest &command,
                                                            const CharacterTransformPublication &previous,
                                                            const CharacterFixedTickInput &input, const Math::Vec3 previousGravityVelocity,
                                                            const float maximumDisplacement) {
        const double seconds = static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0;
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > static_cast<double>(std::numeric_limits<float>::max()))
            return Result<SweepMotionState>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character fixed-tick delta cannot produce a finite movement result."));
        const auto elapsedSeconds = static_cast<float>(seconds);
        SweepMotionState motion{previous.position, command.desiredVelocityMetersPerSecond.value_or(Math::Vec3{}) * elapsedSeconds,
                                elapsedSeconds, previousGravityVelocity};
        if (!Math::IsFinite(motion.remaining) || Math::Length(motion.remaining) > maximumDisplacement)
            return Result<SweepMotionState>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character desired displacement exceeds the finite fixed-tick envelope."));
        return Result<SweepMotionState>::Success(std::move(motion));
    }

    /**
     * @brief Resolves one desired displacement through bounded Horo capsule sweeps and iterative slide.
     *
     * The callback only supplies copied evidence. Character owns ordering, skin-width advancement,
     * contact retention and projection against the canonical normal prefix, so native traversal order
     * cannot change the resulting motion.
     */
    [[nodiscard]] Result<CharacterMovementResult> BuildCapsuleSweepMovementResult(auto &impl, const CharacterMovementRequest &command,
                                                                                  const CharacterTransformPublication &previous,
                                                                                  const CharacterFixedTickInput &input,
                                                                                  const CharacterControllerDescriptor &descriptor,
                                                                                  const Math::Vec3 previousGravityVelocity) {
        const CharacterPhysicsQueryExpectations expected{impl.descriptor.sceneGeneration,        impl.descriptor.identity,
                                                         impl.descriptor.physicsWorld,           impl.descriptor.collisionFilterGeneration,
                                                         impl.descriptor.originGeneration,       input.tick,
                                                         impl.descriptor.physicsSnapshotRevision};
        if (const auto valid = ValidateCharacterPhysicsQueryContext(input.query, expected); valid.HasError())
            return Result<CharacterMovementResult>::Failure(valid.ErrorValue());

        auto result = InitialSweepMovement(command, previous, descriptor, previousGravityVelocity, input.tick);

        auto admitted = AdmitSweepMotion(command, previous, input, previousGravityVelocity,
                                         impl.settings.Values().work.maximumDisplacementMetersPerTick);
        if (admitted.HasError())
            return Result<CharacterMovementResult>::Failure(admitted.ErrorValue());
        auto motion = std::move(admitted).Value();

        if (const auto moved = ResolveDesiredDisplacement(impl, result, command, input, descriptor, motion); moved.HasError())
            return Result<CharacterMovementResult>::Failure(moved.ErrorValue());
        std::optional<CharacterSweepHit> steepSupport;
        auto supportDescriptor = descriptor;
        if ((!previous.grounded && previous.sourceTick != 0) || result.jumpApplied)
            supportDescriptor.maximumStepHeightMeters = 0.0F;
        // A complete step already proved its actual contact, support plane and overlap clearance.
        // Ordinary snap must not replace that proof with the capsule's rounded-edge collision normal.
        if (const bool stepped =
                (static_cast<std::uint16_t>(result.collisions) & static_cast<std::uint16_t>(CharacterCollisionFlags::Step)) != 0;
            !stepped) {
            if (const auto grounded = ResolveGrounding(impl, result, command, input, supportDescriptor, motion.position, steepSupport);
                grounded.HasError())
                return Result<CharacterMovementResult>::Failure(grounded.ErrorValue());
        }
        if (const auto completed =
                ResolvePostSweepMotion(impl, result, command, input, descriptor, supportDescriptor, motion, steepSupport);
            completed.HasError())
            return Result<CharacterMovementResult>::Failure(completed.ErrorValue());
        result.finalPosition = motion.position;
        result.achievedVelocityMetersPerSecond = (motion.position - previous.position) / motion.elapsedSeconds;
        std::ranges::sort(result.contacts.begin(), result.contacts.begin() + result.contactCount, ContactLess);
        return Result<CharacterMovementResult>::Success(std::move(result));
    }
}  // namespace Horo::Character::Detail
