#pragma once

#include "CharacterWorldInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace Horo::Character::Detail {
    constexpr float GroundNormalTolerance = 1.0e-5F;
    constexpr float GroundDistanceTolerance = 1.0e-5F;

    /** @brief Provides the stable response rank used by the Character sweep reducer. */
    [[nodiscard]] std::uint8_t SweepResponseRank(const Physics::PhysicsQueryResponse response) noexcept {
        return response == Physics::PhysicsQueryResponse::Block ? 0U : 1U;
    }

    /** @brief Orders sweep evidence independently of native collector traversal order. */
    [[nodiscard]] bool SweepHitLess(const CharacterSweepHit &left, const CharacterSweepHit &right) noexcept {
        const auto leftBody = left.body.value_or(Physics::BodyHandle{});
        const auto rightBody = right.body.value_or(Physics::BodyHandle{});
        const auto leftKey = std::tuple{left.distanceMeters,
                                        SweepResponseRank(left.response),
                                        left.body.has_value(),
                                        leftBody.world,
                                        leftBody.slot.index,
                                        leftBody.slot.generation,
                                        left.shape.world,
                                        left.shape.slot.index,
                                        left.shape.slot.generation,
                                        left.point.x,
                                        left.point.y,
                                        left.point.z,
                                        left.normal.x,
                                        left.normal.y,
                                        left.normal.z,
                                        left.relativeVelocityMetersPerSecond.x,
                                        left.relativeVelocityMetersPerSecond.y,
                                        left.relativeVelocityMetersPerSecond.z,
                                        left.subshape};
        const auto rightKey = std::tuple{right.distanceMeters,
                                         SweepResponseRank(right.response),
                                         right.body.has_value(),
                                         rightBody.world,
                                         rightBody.slot.index,
                                         rightBody.slot.generation,
                                         right.shape.world,
                                         right.shape.slot.index,
                                         right.shape.slot.generation,
                                         right.point.x,
                                         right.point.y,
                                         right.point.z,
                                         right.normal.x,
                                         right.normal.y,
                                         right.normal.z,
                                         right.relativeVelocityMetersPerSecond.x,
                                         right.relativeVelocityMetersPerSecond.y,
                                         right.relativeVelocityMetersPerSecond.z,
                                         right.subshape};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        return CharacterFastPathOptionalMaterialLess(left.material, right.material);
    }

    /** @brief Orders retained contacts by stable identities and copied geometry. */
    [[nodiscard]] bool ContactLess(const CharacterSurfaceContact &left, const CharacterSurfaceContact &right) noexcept {
        const auto leftBody = left.body.value_or(Physics::BodyHandle{});
        const auto rightBody = right.body.value_or(Physics::BodyHandle{});
        const auto leftKey = std::tuple{left.body.has_value(),
                                        leftBody.world,
                                        leftBody.slot.index,
                                        leftBody.slot.generation,
                                        left.shape.world,
                                        left.shape.slot.index,
                                        left.shape.slot.generation,
                                        left.normal.x,
                                        left.normal.y,
                                        left.normal.z,
                                        left.point.x,
                                        left.point.y,
                                        left.point.z,
                                        left.subshape};
        const auto rightKey = std::tuple{right.body.has_value(),
                                         rightBody.world,
                                         rightBody.slot.index,
                                         rightBody.slot.generation,
                                         right.shape.world,
                                         right.shape.slot.index,
                                         right.shape.slot.generation,
                                         right.normal.x,
                                         right.normal.y,
                                         right.normal.z,
                                         right.point.x,
                                         right.point.y,
                                         right.point.z,
                                         right.subshape};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        if (left.materialSource != right.materialSource)
            return left.materialSource < right.materialSource;
        const auto leftSlot = left.material.slot.Value();
        const auto rightSlot = right.material.slot.Value();
        return std::tie(left.material.asset.Bytes(), left.material.assetGeneration, leftSlot) <
               std::tie(right.material.asset.Bytes(), right.material.assetGeneration, rightSlot);
    }

    /** @brief Reports whether a surface normal is walkable against the controller's explicit up basis. */
    [[nodiscard]] bool IsWalkableGroundNormal(const Math::Vec3 normal, const Math::Vec3 up, const float walkableCosine) noexcept {
        return Math::Dot(normal, up) >= walkableCosine - GroundNormalTolerance;
    }

    /** @brief Computes the stable slope angle for a validated normal and up basis. */
    [[nodiscard]] float GroundSlopeDegrees(const Math::Vec3 normal, const Math::Vec3 up) noexcept {
        const float cosine = std::clamp(Math::Dot(normal, up), -1.0F, 1.0F);
        return std::acos(cosine) * 180.0F / Math::Pi;
    }

    /** @brief Classifies one blocking normal without changing the controller's up basis. */
    [[nodiscard]] CharacterCollisionFlags CollisionFlagForNormal(const Math::Vec3 normal, const Math::Vec3 up, const float walkableCosine) {
        using enum CharacterCollisionFlags;
        const float upDot = Math::Dot(normal, up);
        if (upDot > 0.0F && IsWalkableGroundNormal(normal, up, walkableCosine))
            return Ground;
        if (upDot < 0.0F && -upDot >= walkableCosine - GroundNormalTolerance)
            return Ceiling;
        return Sides;
    }

    /** @brief Retains one stable contact and reports overflow without allocating or reordering state. */
    void RetainSweepContact(CharacterMovementResult &result, const CharacterSweepHit &hit,
                            const CharacterControllerDescriptor &descriptor) {
        if (const auto duplicate = std::ranges::find_if(result.contacts.begin(), result.contacts.begin() + result.contactCount,
                                                        [&hit](const CharacterSurfaceContact &contact) {
            return contact.body == hit.body && contact.shape == hit.shape && contact.subshape == hit.subshape &&
                   contact.normal == hit.normal;
        });
            duplicate != result.contacts.begin() + result.contactCount)
            return;
        if (result.contactCount >= descriptor.maximumContacts) {
            result.truncated = true;
            return;
        }
        auto &contact = result.contacts[result.contactCount++];
        contact.body = hit.body;
        contact.shape = hit.shape;
        contact.point = hit.point;
        contact.normal = hit.normal;
        contact.material = hit.material.value_or(descriptor.defaultMaterial);
        contact.subshape = hit.subshape;
        contact.materialSource = hit.material.has_value() ? CharacterMaterialSource::Query : CharacterMaterialSource::DescriptorFallback;
        contact.penetrationDepthMeters = std::max(0.0F, descriptor.skinWidthMeters - hit.distanceMeters);
    }

    struct SweepBlockSelection final {
        float nearest{};
        bool blocked{};
    };

    struct SweepMotionState final {
        Math::Vec3 position{};
        Math::Vec3 remaining{};
        float elapsedSeconds{};
        Math::Vec3 previousGravityVelocity{};
        std::uint32_t iteration{};
        Math::Vec3 gravityVelocity{};
    };

    /** @brief Applies identical physical eligibility to movement, contact projection and ground selection. */
    [[nodiscard]] bool IsBlockingSweepHit(const CharacterSweepHit &hit, const CharacterControllerDescriptor &descriptor) noexcept {
        const auto &selectors = descriptor.selectors;
        return !hit.trigger && hit.response == Physics::PhysicsQueryResponse::Block &&
               (!selectors.excludedBody || hit.body != selectors.excludedBody) &&
               (!selectors.requiredLayer || hit.layer == selectors.requiredLayer) &&
               (!selectors.requiredProfile || hit.profile == selectors.requiredProfile);
    }

    /** @brief Selects the canonical nearest blocking hit from one sorted response prefix. */
    [[nodiscard]] SweepBlockSelection SelectNearestSweepBlock(const CharacterSweepProbeResult &evidence, const Math::Vec3 direction,
                                                              const float distance,
                                                              const CharacterControllerDescriptor &descriptor) noexcept {
        SweepBlockSelection selection{distance};
        constexpr float NormalEpsilon = 1.0e-5F;
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (!IsBlockingSweepHit(hit, descriptor) || Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                continue;
            // The active prefix is sorted by distance before this reducer runs, so the first valid
            // blocking hit is the nearest one and no later evidence can change the selection.
            selection.nearest = hit.distanceMeters;
            selection.blocked = true;
            break;
        }
        return selection;
    }

    /** @brief Clips a surface without manufacturing uphill travel on a non-walkable normal. */
    [[nodiscard]] Math::Vec3 ClipSlopeMotion(const Math::Vec3 motion, const Math::Vec3 normal,
                                             const CharacterControllerDescriptor &descriptor, const float walkableCosine) noexcept {
        const float intoSurface = Math::Dot(motion, normal);
        if (intoSurface >= 0.0F)
            return motion;
        const float upDot = Math::Dot(normal, descriptor.up);
        const float vertical = Math::Dot(motion, descriptor.up);
        Math::Vec3 horizontal = motion - descriptor.up * vertical;
        if (upDot > GroundNormalTolerance && IsWalkableGroundNormal(normal, descriptor.up, walkableCosine) &&
            descriptor.preserveHorizontalSpeedOnSlopes)
            return horizontal - descriptor.up * (Math::Dot(horizontal, normal) / upDot);
        if (upDot > 0.0F && !IsWalkableGroundNormal(normal, descriptor.up, walkableCosine)) {
            const Math::Vec3 horizontalNormal = normal - descriptor.up * upDot;
            const float lengthSquared = Math::LengthSquared(horizontalNormal);
            const float intoHorizontal = Math::Dot(horizontal, horizontalNormal);
            if (intoHorizontal < 0.0F && lengthSquared > GroundNormalTolerance * GroundNormalTolerance)
                horizontal -= horizontalNormal * (intoHorizontal / lengthSquared);
            Math::Vec3 clipped = horizontal + descriptor.up * vertical;
            // Downward motion may slide along a steep face, but projection must never create ascent.
            const float intoClipped = Math::Dot(clipped, normal);
            if (intoClipped < 0.0F)
                clipped -= normal * intoClipped;
            return clipped;
        }
        return motion - normal * intoSurface;
    }

    /** @brief Advances to a blocking skin boundary and projects remaining travel against its canonical normals. */
    void ApplySweepBlock(CharacterMovementResult &result, const CharacterSweepProbeResult &evidence,
                         const CharacterControllerDescriptor &descriptor, const Math::Vec3 direction, const float nearest,
                         const float maximumAscent, SweepMotionState &motion) {
        auto &position = motion.position;
        auto &remaining = motion.remaining;
        constexpr float DistanceEpsilon = 1.0e-5F;
        constexpr float NormalEpsilon = 1.0e-5F;
        // An obstructed gravity slide starts from rest next tick, independently of the
        // retained contact capacity. Never accumulate pressure behind a clipped displacement.
        motion.gravityVelocity = {};
        const float travel = std::max(0.0F, nearest - descriptor.skinWidthMeters);
        position += direction * travel;
        remaining -= direction * travel;
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);

        std::array<Math::Vec3, MaximumCharacterSweepHits> activeNormals{};
        std::uint32_t activeNormalCount{};
        bool steepContact{};
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (!IsBlockingSweepHit(hit, descriptor) || hit.distanceMeters > nearest + DistanceEpsilon ||
                Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                continue;
            steepContact = steepContact || (Math::Dot(hit.normal, descriptor.up) > 0.0F &&
                                            !IsWalkableGroundNormal(hit.normal, descriptor.up, walkableCosine));
            result.collisions = result.collisions | CollisionFlagForNormal(hit.normal, descriptor.up, walkableCosine);
            RetainSweepContact(result, hit, descriptor);
            if (activeNormalCount < activeNormals.size())
                activeNormals[activeNormalCount++] = hit.normal;
        }

        for (std::uint32_t index{}; index < activeNormalCount; ++index) {
            remaining = ClipSlopeMotion(remaining, activeNormals[index], descriptor, walkableCosine);
        }
        if (const float ascent = Math::Dot(remaining, descriptor.up); steepContact && ascent > maximumAscent)
            remaining -= descriptor.up * (ascent - maximumAscent);
        // A later plane can invalidate an earlier constraint in a corner. Fail closed rather
        // than preserving speed through geometry or repeatedly amplifying ramp projections.
        for (std::uint32_t index{}; index < activeNormalCount; ++index) {
            if (Math::Dot(remaining, activeNormals[index]) < -NormalEpsilon)
                remaining = {};
        }
    }

    /** @brief Clears all optional support evidence while retaining the explicit up basis. */
    void ClearGroundEvidence(CharacterMovementResult &result, const Math::Vec3 up) noexcept {
        result.grounded = false;
        result.groundSlopeDegrees = 0.0F;
        result.groundNormal = up;
        result.groundMaterial.reset();
        result.groundSubshape.reset();
        result.groundMaterialSource = CharacterMaterialSource::Query;
        result.groundPoint = {};
        result.groundBody.reset();
        result.groundShape = {};
        result.groundDistanceMeters = 0.0F;
        result.groundRelativeVelocityMetersPerSecond = {};
        result.platformAttached = false;
        result.groundingRevalidationRequired = true;
    }

    /** @brief Returns whether this command is allowed to pull a controller down onto a nearby floor. */
    [[nodiscard]] bool MaySnapToGround(const CharacterMovementRequest &command, const Math::Vec3 up) noexcept {
        if (command.jumpRequested)
            return false;
        return !command.desiredVelocityMetersPerSecond.has_value() ||
               Math::Dot(*command.desiredVelocityMetersPerSecond, up) <= GroundNormalTolerance;
    }

    /** @brief Computes the bounded downward probe length from the controller's snap policy. */
    [[nodiscard]] float GroundProbeDistance(const CharacterControllerDescriptor &descriptor) noexcept {
        const float snapDistance = std::max(descriptor.skinWidthMeters, descriptor.maximumStepHeightMeters);
        return snapDistance + descriptor.skinWidthMeters + GroundDistanceTolerance;
    }

    /** @brief Finds a stable walkable support hit from one sorted downward probe. */
    [[nodiscard]] std::optional<CharacterSweepHit> SelectGroundHit(const CharacterSweepProbeResult &evidence,
                                                                   const CharacterControllerDescriptor &descriptor) {
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);
        const float snapDistance = std::max(descriptor.skinWidthMeters, descriptor.maximumStepHeightMeters);
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const CharacterSweepHit &hit = evidence.hits[index];
            if (!IsBlockingSweepHit(hit, descriptor) || hit.distanceMeters > snapDistance + GroundDistanceTolerance ||
                !IsWalkableGroundNormal(hit.normal, descriptor.up, walkableCosine))
                continue;
            return hit;
        }
        return std::nullopt;
    }

    /** @brief Copies one selected Physics support independently of the retained contact prefix. */
    void PublishGroundSupport(CharacterMovementResult &result, const CharacterSweepHit &support,
                              const CharacterControllerDescriptor &descriptor, const float snap) {
        result.grounded = true;
        result.groundSlopeDegrees = GroundSlopeDegrees(support.normal, descriptor.up);
        result.groundNormal = support.normal;
        result.groundMaterial = support.material.value_or(descriptor.defaultMaterial);
        result.groundSubshape = support.subshape;
        result.groundMaterialSource =
            support.material.has_value() ? CharacterMaterialSource::Query : CharacterMaterialSource::DescriptorFallback;
        result.groundPoint = support.point;
        result.groundBody = support.body;
        result.groundShape = support.shape;
        result.groundDistanceMeters = support.distanceMeters - snap;
        result.groundRelativeVelocityMetersPerSecond = support.relativeVelocityMetersPerSecond;
        result.collisions = result.collisions | CharacterCollisionFlags::Ground;
        result.groundingRevalidationRequired = false;
        RetainSweepContact(result, support, descriptor);
    }

    /** @brief Resolves bounded floor classification and downward snap after ordinary movement. */
    [[nodiscard]] Result<void> ResolveGrounding(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                Math::Vec3 &position, std::optional<CharacterSweepHit> &steepSupport) {
        ClearGroundEvidence(result, descriptor.up);
        if (!MaySnapToGround(command, descriptor.up))
            return Result<void>::Success();

        const Math::Vec3 down = descriptor.up * -1.0F;
        const CharacterSweepProbeRequest request{command.controller,
                                                 impl.descriptor.sceneGeneration,
                                                 impl.descriptor.identity,
                                                 impl.descriptor.physicsWorld,
                                                 descriptor.capsule,
                                                 position,
                                                 descriptor.up,
                                                 down,
                                                 GroundProbeDistance(descriptor),
                                                 descriptor.collisionProfile,
                                                 descriptor.queryChannel,
                                                 impl.settings.Values().work.maximumMovementIterations,
                                                 descriptor.selectors};
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return budget;
        if (input.metrics != nullptr)
            ++input.metrics->snapshot.queries;
        auto probe = input.query.sweep(input.query.context, request);
        if (const auto continuation = ValidateTickQueryContinuation(impl, probe); continuation.HasError())
            return continuation;
        CharacterSweepProbeResult evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return valid;
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        // Never snap through the nearest blocking steep surface onto a deeper floor.
        const auto nearestBlock = std::ranges::find_if(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount,
                                                       [&descriptor](const CharacterSweepHit &hit) {
            return IsBlockingSweepHit(hit, descriptor);
        });
        const float walkableCosine = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F);
        if (nearestBlock != evidence.hits.begin() + evidence.hitCount && Math::Dot(nearestBlock->normal, descriptor.up) > 0.0F &&
            !IsWalkableGroundNormal(nearestBlock->normal, descriptor.up, walkableCosine)) {
            if (nearestBlock->distanceMeters <= descriptor.skinWidthMeters + GroundDistanceTolerance) {
                steepSupport = *nearestBlock;
                result.collisions = result.collisions | CharacterCollisionFlags::Sides;
                RetainSweepContact(result, *nearestBlock, descriptor);
            }
            return Result<void>::Success();
        }
        const auto support = SelectGroundHit(evidence, descriptor);
        if (!support.has_value())
            return Result<void>::Success();

        const float snap = std::max(0.0F, support->distanceMeters - descriptor.skinWidthMeters);
        position += down * snap;
        PublishGroundSupport(result, *support, descriptor, snap);
        return Result<void>::Success();
    }

    /** @brief Resolves one bounded sweep query and returns whether another iteration may continue. */
    [[nodiscard]] Result<bool> ResolveCapsuleSweepIteration(auto &impl, CharacterMovementResult &result,
                                                            const CharacterMovementRequest &command, const CharacterFixedTickInput &input,
                                                            const CharacterControllerDescriptor &descriptor, SweepMotionState &motion,
                                                            const std::uint32_t iteration) {
        const float distance = Math::Length(motion.remaining);
        if (!std::isfinite(distance) || distance <= descriptor.minimumMoveDistanceMeters)
            return Result<bool>::Success(false);
        const Math::Vec3 direction = motion.remaining / distance;
        const CharacterSweepProbeRequest request{command.controller,
                                                 impl.descriptor.sceneGeneration,
                                                 impl.descriptor.identity,
                                                 impl.descriptor.physicsWorld,
                                                 descriptor.capsule,
                                                 motion.position,
                                                 descriptor.up,
                                                 direction,
                                                 distance,
                                                 descriptor.collisionProfile,
                                                 descriptor.queryChannel,
                                                 iteration,
                                                 descriptor.selectors};
        if (const auto budget = ReserveTickQuery(impl); budget.HasError())
            return Result<bool>::Failure(budget.ErrorValue());
        if (input.metrics != nullptr) {
            ++input.metrics->snapshot.queries;
            ++input.metrics->snapshot.movementIterations;
        }
        auto probe = input.query.sweep(input.query.context, request);
        if (const auto continuation = ValidateTickQueryContinuation(impl, probe); continuation.HasError())
            return Result<bool>::Failure(continuation.ErrorValue());
        CharacterSweepProbeResult evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        const auto selection = SelectNearestSweepBlock(evidence, direction, distance, descriptor);
        if (!selection.blocked) {
            motion.position += motion.remaining;
            motion.remaining = {};
            return Result<bool>::Success(false);
        }
        const float seconds = static_cast<float>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0F;
        const float maximumAscent =
            std::max(0.0F, Math::Dot(command.desiredVelocityMetersPerSecond.value_or(Math::Vec3{}), descriptor.up)) * seconds;
        ApplySweepBlock(result, evidence, descriptor, direction, selection.nearest, maximumAscent, motion);
        return Result<bool>::Success(true);
    }

    /** @brief Stages gravity continuation on one steep face within the remaining fixed-tick sweep budget. */
    [[nodiscard]] Result<void> ResolveSteepSliding(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                   const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                   SweepMotionState &motion, std::optional<CharacterSweepHit> &steepSupport) {
        if (!steepSupport.has_value() || descriptor.steepSlopePolicy != CharacterSteepSlopePolicy::Slide)
            return Result<void>::Success();
        const Math::Vec3 normal = steepSupport->normal;
        const Math::Vec3 tangentGravity = descriptor.gravity - normal * Math::Dot(descriptor.gravity, normal);
        // A custom gravity basis must still never turn the steep-slide policy into a climb.
        if (Math::Dot(tangentGravity, descriptor.up) >= 0.0F)
            return Result<void>::Success();
        if (motion.iteration >= impl.settings.Values().work.maximumMovementIterations)
            return Result<void>::Failure(
                MakeError(CharacterErrors::CapacityExceeded, "Steep sliding exhausted the movement iteration budget."));
        Math::Vec3 initialVelocity = motion.previousGravityVelocity - normal * Math::Dot(motion.previousGravityVelocity, normal);
        if (Math::Dot(initialVelocity, descriptor.up) > 0.0F)
            initialVelocity = {};
        const Math::Vec3 finalVelocity = initialVelocity + tangentGravity * motion.elapsedSeconds;
        motion.gravityVelocity = finalVelocity;
        motion.remaining = (initialVelocity + finalVelocity) * (motion.elapsedSeconds * 0.5F);
        if (!Math::IsFinite(motion.remaining))
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        auto slideDescriptor = descriptor;
        slideDescriptor.minimumMoveDistanceMeters = 0.0F;
        for (; motion.iteration < impl.settings.Values().work.maximumMovementIterations; ++motion.iteration) {
            const auto resolved = ResolveCapsuleSweepIteration(impl, result, command, input, slideDescriptor, motion, motion.iteration);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                break;
        }
        steepSupport.reset();
        if (const auto grounded = ResolveGrounding(impl, result, command, input, descriptor, motion.position, steepSupport);
            grounded.HasError())
            return Result<void>::Failure(grounded.ErrorValue());
        if (steepSupport.has_value()) {
            const float walkableCosine = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F);
            const Math::Vec3 continuation = ClipSlopeMotion(motion.gravityVelocity, steepSupport->normal, descriptor, walkableCosine);
            if (Math::Dot(continuation, descriptor.up) <= GroundNormalTolerance)
                result.gravityVelocityMetersPerSecond = continuation;
        }
        return Result<void>::Success();
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

        CharacterMovementResult result;
        result.controller = command.controller;
        result.tick = input.tick;
        result.sequence = command.sequence;
        result.finalPosition = previous.position;
        result.finalHeading = command.desiredHeading.value_or(previous.heading);
        result.up = descriptor.up;
        ClearGroundEvidence(result, descriptor.up);

        const double seconds = static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0;
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > static_cast<double>(std::numeric_limits<float>::max()))
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character fixed-tick delta cannot produce a finite movement result."));
        const auto elapsedSeconds = static_cast<float>(seconds);
        SweepMotionState motion{previous.position, command.desiredVelocityMetersPerSecond.value_or(Math::Vec3{}) * elapsedSeconds,
                                elapsedSeconds, previousGravityVelocity};
        if (!Math::IsFinite(motion.remaining))
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character desired displacement is not finite."));

        for (; motion.iteration < impl.settings.Values().work.maximumMovementIterations; ++motion.iteration) {
            if (Math::LengthSquared(motion.remaining) <= descriptor.minimumMoveDistanceMeters * descriptor.minimumMoveDistanceMeters)
                break;
            const auto resolved = ResolveCapsuleSweepIteration(impl, result, command, input, descriptor, motion, motion.iteration);
            if (resolved.HasError())
                return Result<CharacterMovementResult>::Failure(resolved.ErrorValue());
            if (!resolved.Value()) {
                ++motion.iteration;
                break;
            }
        }
        std::optional<CharacterSweepHit> steepSupport;
        if (const auto grounded = ResolveGrounding(impl, result, command, input, descriptor, motion.position, steepSupport);
            grounded.HasError())
            return Result<CharacterMovementResult>::Failure(grounded.ErrorValue());
        if (const auto slide = ResolveSteepSliding(impl, result, command, input, descriptor, motion, steepSupport); slide.HasError())
            return Result<CharacterMovementResult>::Failure(slide.ErrorValue());
        result.finalPosition = motion.position;
        result.achievedVelocityMetersPerSecond = (motion.position - previous.position) / elapsedSeconds;
        std::ranges::sort(result.contacts.begin(), result.contacts.begin() + result.contactCount, ContactLess);
        return Result<CharacterMovementResult>::Success(std::move(result));
    }
}  // namespace Horo::Character::Detail
