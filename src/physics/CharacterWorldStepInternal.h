#pragma once

/** @file
 * @brief Target-private checked step admission and complete capsule clearance certification.
 * Rejected stages preserve ordinary movement; copied evidence never grants publication authority.
 */
#include "CharacterWorldSweepInternal.h"

namespace Horo::Character::Detail {
    /** @brief Borrows one synchronous step operation's immutable query policy. */
    struct StepQueryContext final {
        const CharacterMovementRequest &command;
        const CharacterFixedTickInput &input;
        const CharacterControllerDescriptor &descriptor;
    };

    /** @brief Owns one bounded cast's geometry without retaining native state. */
    struct StepCast final {
        Math::Vec3 position;
        Math::Vec3 direction;
        float distance{};
        std::uint32_t iteration{};
    };

    /** @brief Owns the actual landing contact and center; lookahead never changes requested travel. */
    struct StepLanding final {
        CharacterSweepHit floor;
        Math::Vec3 position;
        float descent{};
    };

    enum class SweepPurpose {
        Step,
        Movement
    };

    /** @brief Executes one admitted synchronous cast, retaining only validated copied evidence after owner-liveness checks. */
    [[nodiscard]] Result<CharacterSweepProbeResult> ReadCapsuleSweep(auto &impl, const StepQueryContext &query, const StepCast &cast,
                                                                     const SweepPurpose purpose) {
        const auto &[command, input, descriptor] = query;
        const auto &[position, direction, distance, iteration] = cast;
        const CharacterSweepProbeRequest request{command.controller,
                                                 impl.descriptor.sceneGeneration,
                                                 impl.descriptor.identity,
                                                 impl.descriptor.physicsWorld,
                                                 descriptor.capsule,
                                                 position,
                                                 descriptor.up,
                                                 direction,
                                                 distance,
                                                 descriptor.collisionProfile,
                                                 descriptor.queryChannel,
                                                 iteration,
                                                 descriptor.selectors};
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return Result<CharacterSweepProbeResult>::Failure(budget.ErrorValue());
        if (input.metrics != nullptr) {
            ++input.metrics->snapshot.queries;
            if (purpose == SweepPurpose::Movement)
                ++input.metrics->snapshot.movementIterations;
        }
        auto probe = input.query.sweep(input.query.context, request);
        if (const auto valid = ValidateTickQueryContinuation(impl, probe); valid.HasError())
            return Result<CharacterSweepProbeResult>::Failure(valid.ErrorValue());
        auto evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return Result<CharacterSweepProbeResult>::Failure(valid.ErrorValue());
        impl.debug.RecordSweep(request, evidence,
                               purpose == SweepPurpose::Movement ? CharacterDebugProbePurpose::Movement : CharacterDebugProbePurpose::Step);
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        return Result<CharacterSweepProbeResult>::Success(std::move(evidence));
    }

    /** @brief Reads step-stage evidence without counting an ordinary movement iteration. */
    [[nodiscard]] Result<CharacterSweepProbeResult> ProbeStep(auto &impl, const StepQueryContext &query, const StepCast &cast) {
        return ReadCapsuleSweep(impl, query, cast, SweepPurpose::Step);
    }

    /** @brief Requires the lookahead to prove the actual contact's plane and exact stable surface identity. */
    [[nodiscard]] bool MatchesStepPlane(const CharacterSweepHit &contact, const std::optional<CharacterSweepHit> &support,
                                        const SweepBlockSelection block) noexcept {
        return support && block.blocked && support->distanceMeters <= block.nearest + GroundDistanceTolerance &&
               support->body == contact.body && support->shape == contact.shape && support->subshape == contact.subshape &&
               std::abs(Math::Dot(support->point - contact.point, support->normal)) <= GroundDistanceTolerance;
    }

    /** @brief Certifies a rounded-edge contact only with a clear, coplanar, walkable point on the same surface. */
    [[nodiscard]] Result<std::optional<CharacterSweepHit>> CertifyStepFloor(auto &impl, const StepQueryContext &query,
                                                                            const CharacterSweepProbeResult &evidence, const StepCast &cast,
                                                                            const Math::Vec3 forward) {
        const auto &descriptor = query.descriptor;
        const auto block = SelectNearestSweepBlock(evidence, cast.direction, cast.distance, descriptor);
        if (!block.blocked)
            return Result<std::optional<CharacterSweepHit>>::Success(std::nullopt);
        if (const auto walkable = SelectGroundHit(evidence, descriptor);
            walkable && walkable->distanceMeters <= block.nearest + GroundDistanceTolerance)
            return Result<std::optional<CharacterSweepHit>>::Success(*walkable);
        const auto nearest = std::ranges::find_if(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount,
                                                  [&descriptor, &cast](const CharacterSweepHit &hit) {
            return IsBlockingSweepHit(hit, descriptor) && Math::Dot(hit.normal, cast.direction) < -GroundNormalTolerance;
        });
        if (nearest == evidence.hits.begin() + evidence.hitCount || Math::Dot(nearest->normal, descriptor.up) <= 0.0F)
            return Result<std::optional<CharacterSweepHit>>::Success(std::nullopt);
        const float lift =
            std::max(0.0F, descriptor.maximumStepHeightMeters + descriptor.skinWidthMeters + GroundDistanceTolerance - cast.distance);
        const Math::Vec3 raised = cast.position + descriptor.up * lift;
        const auto path = ProbeStep(impl, query, {raised, forward, descriptor.capsule.radiusMeters, cast.iteration});
        if (path.HasError())
            return Result<std::optional<CharacterSweepHit>>::Failure(path.ErrorValue());
        if (SelectNearestSweepBlock(path.Value(), forward, descriptor.capsule.radiusMeters, descriptor).blocked)
            return Result<std::optional<CharacterSweepHit>>::Success(std::nullopt);
        const auto ahead =
            ProbeStep(impl, query,
                      {raised + forward * descriptor.capsule.radiusMeters, -descriptor.up, lift + cast.distance, cast.iteration});
        if (ahead.HasError())
            return Result<std::optional<CharacterSweepHit>>::Failure(ahead.ErrorValue());
        const auto support = SelectGroundHit(ahead.Value(), descriptor);
        if (const auto aheadBlock = SelectNearestSweepBlock(ahead.Value(), -descriptor.up, lift + cast.distance, descriptor);
            !MatchesStepPlane(*nearest, support, aheadBlock))
            return Result<std::optional<CharacterSweepHit>>::Success(std::nullopt);
        auto certified = *nearest;
        certified.normal = support->normal;
        return Result<std::optional<CharacterSweepHit>>::Success(certified);
    }

    /** @brief Requires touching walkable support before a side obstruction can become a stair. */
    [[nodiscard]] Result<bool> HasStepSupport(auto &impl, const StepQueryContext &query, const SweepMotionState &motion,
                                              const Math::Vec3 forward) {
        const auto &descriptor = query.descriptor;
        const StepCast cast{motion.position, -descriptor.up, descriptor.skinWidthMeters + GroundDistanceTolerance, motion.iteration};
        const auto ground = ProbeStep(impl, query, cast);
        if (ground.HasError())
            return Result<bool>::Failure(ground.ErrorValue());
        const auto floor = CertifyStepFloor(impl, query, ground.Value(), cast, forward);
        if (floor.HasError())
            return Result<bool>::Failure(floor.ErrorValue());
        return Result<bool>::Success(floor.Value().has_value() && floor.Value()->distanceMeters <= cast.distance);
    }

    /** @brief Stages the exact requested up/forward path without changing ordinary movement on rejection. */
    [[nodiscard]] Result<std::optional<Math::Vec3>> AdvanceStepPath(auto &impl, const StepQueryContext &query,
                                                                    const SweepMotionState &motion, const Math::Vec3 horizontal) {
        const auto &descriptor = query.descriptor;
        const float lift = descriptor.maximumStepHeightMeters;
        const auto up = ProbeStep(impl, query, {motion.position, descriptor.up, lift + descriptor.skinWidthMeters, motion.iteration});
        if (up.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(up.ErrorValue());
        if (SelectNearestSweepBlock(up.Value(), descriptor.up, lift + descriptor.skinWidthMeters, descriptor).blocked)
            return Result<std::optional<Math::Vec3>>::Success(std::nullopt);
        const Math::Vec3 raised = motion.position + descriptor.up * lift;
        const float distance = Math::Length(horizontal);
        const Math::Vec3 forward = horizontal / distance;
        const auto path = ProbeStep(impl, query, {raised, forward, distance + descriptor.skinWidthMeters, motion.iteration});
        if (path.HasError())
            return Result<std::optional<Math::Vec3>>::Failure(path.ErrorValue());
        if (SelectNearestSweepBlock(path.Value(), forward, distance + descriptor.skinWidthMeters, descriptor).blocked)
            return Result<std::optional<Math::Vec3>>::Success(std::nullopt);
        return Result<std::optional<Math::Vec3>>::Success(raised + horizontal);
    }

    /** @brief Checks the actual contacted elevation and center against the authored step limit. */
    [[nodiscard]] bool IsStepRiseValid(const StepLanding &landing, const Math::Vec3 start,
                                       const CharacterControllerDescriptor &descriptor) noexcept {
        const float rise = Math::Dot(landing.position - start, descriptor.up);
        const float bottom = descriptor.capsule.cylindricalHalfHeightMeters + descriptor.capsule.radiusMeters;
        const float surfaceRise = Math::Dot(landing.floor.point - start, descriptor.up) + bottom + descriptor.skinWidthMeters;
        return rise > GroundDistanceTolerance && rise <= descriptor.maximumStepHeightMeters + GroundDistanceTolerance &&
               surfaceRise <= descriptor.maximumStepHeightMeters + GroundDistanceTolerance;
    }

    /** @brief Resolves copied landing evidence while keeping the actual capsule's requested horizontal endpoint. */
    [[nodiscard]] Result<std::optional<StepLanding>> FindStepLanding(auto &impl, const StepQueryContext &query,
                                                                     const SweepMotionState &motion, const Math::Vec3 advanced,
                                                                     const Math::Vec3 forward) {
        const auto &descriptor = query.descriptor;
        const StepCast cast{advanced, -descriptor.up,
                            descriptor.maximumStepHeightMeters + descriptor.skinWidthMeters + GroundDistanceTolerance, motion.iteration};
        const auto landing = ProbeStep(impl, query, cast);
        if (landing.HasError())
            return Result<std::optional<StepLanding>>::Failure(landing.ErrorValue());
        const auto floor = CertifyStepFloor(impl, query, landing.Value(), cast, forward);
        if (floor.HasError())
            return Result<std::optional<StepLanding>>::Failure(floor.ErrorValue());
        if (!floor.Value())
            return Result<std::optional<StepLanding>>::Success(std::nullopt);
        const float descent = std::max(0.0F, floor.Value()->distanceMeters - descriptor.skinWidthMeters);
        const StepLanding candidate{*floor.Value(), advanced - descriptor.up * descent, descent};
        if (!IsStepRiseValid(candidate, motion.position, descriptor))
            return Result<std::optional<StepLanding>>::Success(std::nullopt);
        return Result<std::optional<StepLanding>>::Success(candidate);
    }

    /** @brief Requires complete capsule clearance before a staged landing can be published. */
    [[nodiscard]] Result<bool> ClearStepLanding(auto &impl, const StepQueryContext &query, const SweepMotionState &motion,
                                                const Math::Vec3 candidate) {
        const auto &[command, input, descriptor] = query;
        const CharacterOverlapProbeRequest clearance{command.controller,
                                                     impl.descriptor.sceneGeneration,
                                                     impl.descriptor.identity,
                                                     impl.descriptor.physicsWorld,
                                                     descriptor.capsule,
                                                     candidate,
                                                     descriptor.up,
                                                     descriptor.collisionProfile,
                                                     descriptor.queryChannel,
                                                     motion.iteration,
                                                     descriptor.selectors};
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return Result<bool>::Failure(budget.ErrorValue());
        if (input.metrics != nullptr)
            ++input.metrics->snapshot.queries;
        const auto overlap = input.query.overlap(input.query.context, clearance);
        if (const auto valid = ValidateTickQueryContinuation(impl, overlap); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateCharacterOverlapProbeResult(overlap.Value()); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        impl.debug.RecordOverlap(clearance, overlap.Value(), CharacterDebugProbePurpose::Step);
        return Result<bool>::Success(overlap.Value().overlapCount == 0);
    }

    /** @brief Commits a complete checked step; every rejected stage preserves ordinary movement. */
    [[nodiscard]] Result<bool> TryCapsuleStep(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                              const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                              SweepMotionState &motion) {
        const auto rejected = Result<bool>::Success(false);
        if (descriptor.maximumStepHeightMeters <= 0.0F || !MaySnapToGround(command, result, descriptor.up))
            return rejected;
        const float vertical = Math::Dot(motion.remaining, descriptor.up);
        if (vertical > GroundDistanceTolerance)
            return rejected;
        const Math::Vec3 horizontal = motion.remaining - descriptor.up * vertical;
        const float distance = Math::Length(horizontal);
        if (distance <= descriptor.minimumMoveDistanceMeters)
            return rejected;
        const Math::Vec3 forward = horizontal / distance;
        const StepQueryContext query{command, input, descriptor};
        const auto supported = HasStepSupport(impl, query, motion, forward);
        if (supported.HasError())
            return Result<bool>::Failure(supported.ErrorValue());
        if (!supported.Value())
            return rejected;
        const auto advanced = AdvanceStepPath(impl, query, motion, horizontal);
        if (advanced.HasError())
            return Result<bool>::Failure(advanced.ErrorValue());
        if (!advanced.Value())
            return rejected;
        const auto landing = FindStepLanding(impl, query, motion, *advanced.Value(), forward);
        if (landing.HasError())
            return Result<bool>::Failure(landing.ErrorValue());
        if (!landing.Value())
            return rejected;
        const auto clear = ClearStepLanding(impl, query, motion, landing.Value()->position);
        if (clear.HasError())
            return Result<bool>::Failure(clear.ErrorValue());
        if (!clear.Value())
            return rejected;
        motion.position = landing.Value()->position;
        motion.remaining = {};
        result.collisions = result.collisions | CharacterCollisionFlags::Step;
        PublishGroundSupport(result, landing.Value()->floor, descriptor, landing.Value()->descent);
        return Result<bool>::Success(true);
    }

    /** @brief Includes the authored riser range without letting capsule curvature bypass final height checks. */
    [[nodiscard]] bool HasLowStepObstacle(const CharacterSweepProbeResult &evidence, const CharacterControllerDescriptor &descriptor,
                                          const SweepMotionState &motion, const Math::Vec3 direction, const float nearest) {
        const float lowerCenter = -descriptor.capsule.cylindricalHalfHeightMeters;
        const float eligibleHeight = descriptor.maximumStepHeightMeters - descriptor.capsule.cylindricalHalfHeightMeters -
                                     descriptor.capsule.radiusMeters - descriptor.skinWidthMeters;
        const float height = std::max(lowerCenter, eligibleHeight);
        return std::ranges::any_of(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount,
                                   [&descriptor, &motion, direction, nearest, height](const CharacterSweepHit &hit) {
            return IsBlockingSweepHit(hit, descriptor) && hit.distanceMeters <= nearest + GroundDistanceTolerance &&
                   Math::Dot(hit.normal, direction) < -GroundNormalTolerance &&
                   Math::Dot(hit.point - motion.position, descriptor.up) <= height + GroundDistanceTolerance;
        });
    }

    /** @brief Removes manufactured ascent after rejection and stops any resulting motion back into the obstacle. */
    void ClampRejectedStepMotion(const CharacterSweepProbeResult &evidence, const CharacterControllerDescriptor &descriptor,
                                 const float nearest, const float maximumAscent, SweepMotionState &motion) {
        const float ascent = Math::Dot(motion.remaining, descriptor.up);
        if (ascent <= maximumAscent)
            return;
        motion.remaining -= descriptor.up * (ascent - maximumAscent);
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (IsBlockingSweepHit(hit, descriptor) && hit.distanceMeters <= nearest + GroundDistanceTolerance &&
                Math::Dot(motion.remaining, hit.normal) < -GroundNormalTolerance)
                motion.remaining = {};
        }
    }

}  // namespace Horo::Character::Detail
