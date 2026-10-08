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
        bool airbornePass{};
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
            if (const float intoHorizontal = Math::Dot(horizontal, horizontalNormal);
                intoHorizontal < 0.0F && lengthSquared > GroundNormalTolerance * GroundNormalTolerance)
                horizontal -= horizontalNormal * (intoHorizontal / lengthSquared);
            Math::Vec3 clipped = horizontal + descriptor.up * vertical;
            // Downward motion may slide along a steep face, but projection must never create ascent.
            if (const float intoClipped = Math::Dot(clipped, normal); intoClipped < 0.0F)
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
        if (!motion.airbornePass)
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
            if (motion.airbornePass) {
                const float forbidden = Math::Dot(motion.gravityVelocity, activeNormals[index]);
                if (forbidden < 0.0F)
                    motion.gravityVelocity -= activeNormals[index] * forbidden;
            }
        }
        if (const float ascent = Math::Dot(remaining, descriptor.up); steepContact && ascent > maximumAscent)
            remaining -= descriptor.up * (ascent - maximumAscent);
        // A later plane can invalidate an earlier constraint in a corner. Fail closed rather
        // than preserving speed through geometry or repeatedly amplifying ramp projections.
        for (std::uint32_t index{}; index < activeNormalCount; ++index) {
            if (Math::Dot(remaining, activeNormals[index]) < -NormalEpsilon)
                remaining = {};
            if (motion.airbornePass && Math::Dot(motion.gravityVelocity, activeNormals[index]) < -NormalEpsilon)
                motion.gravityVelocity = {};
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
    [[nodiscard]] bool MaySnapToGround(const CharacterMovementRequest &command, const CharacterMovementResult &result,
                                       const Math::Vec3 up) noexcept {
        if (Math::Dot(result.gravityVelocityMetersPerSecond, up) > GroundNormalTolerance)
            return false;
        return !command.desiredVelocityMetersPerSecond.has_value() ||
               Math::Dot(*command.desiredVelocityMetersPerSecond, up) <= GroundNormalTolerance;
    }

    /** @brief Computes the bounded downward probe length from the controller's snap policy. */
    [[nodiscard]] float GroundProbeDistance(const CharacterControllerDescriptor &descriptor) noexcept {
        const float snapDistance = descriptor.maximumStepHeightMeters + descriptor.skinWidthMeters;
        return snapDistance + descriptor.skinWidthMeters + GroundDistanceTolerance;
    }

    /** @brief Finds a stable walkable support hit from one sorted downward probe. */
    [[nodiscard]] std::optional<CharacterSweepHit> SelectGroundHit(const CharacterSweepProbeResult &evidence,
                                                                   const CharacterControllerDescriptor &descriptor) {
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);
        const float snapDistance = descriptor.maximumStepHeightMeters + descriptor.skinWidthMeters;
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

    /** @brief Retains nearby steep support and prevents grounding through a nearer blocking steep surface. */
    [[nodiscard]] bool RetainNearestSteepSupport(const CharacterSweepProbeResult &evidence, const CharacterControllerDescriptor &descriptor,
                                                 CharacterMovementResult &result, std::optional<CharacterSweepHit> &steepSupport) {
        const auto nearestBlock = std::ranges::find_if(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount,
                                                       [&descriptor](const CharacterSweepHit &hit) {
            return IsBlockingSweepHit(hit, descriptor);
        });
        if (const float walkableCosine = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F);
            nearestBlock != evidence.hits.begin() + evidence.hitCount && Math::Dot(nearestBlock->normal, descriptor.up) > 0.0F &&
            !IsWalkableGroundNormal(nearestBlock->normal, descriptor.up, walkableCosine)) {
            if (nearestBlock->distanceMeters <= descriptor.skinWidthMeters + GroundDistanceTolerance) {
                steepSupport = *nearestBlock;
                result.collisions = result.collisions | CharacterCollisionFlags::Sides;
                RetainSweepContact(result, *nearestBlock, descriptor);
            }
            return true;
        }
        return false;
    }

    /** @brief Resolves bounded floor classification and downward snap after ordinary movement. */
    [[nodiscard]] Result<void> ResolveGrounding(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                Math::Vec3 &position, std::optional<CharacterSweepHit> &steepSupport) {
        ClearGroundEvidence(result, descriptor.up);
        if (!MaySnapToGround(command, result, descriptor.up))
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
        if (RetainNearestSteepSupport(evidence, descriptor, result, steepSupport))
            return Result<void>::Success();
        const auto support = SelectGroundHit(evidence, descriptor);
        if (!support.has_value())
            return Result<void>::Success();

        const float snap = std::max(0.0F, support->distanceMeters - descriptor.skinWidthMeters);
        position += down * snap;
        PublishGroundSupport(result, *support, descriptor, snap);
        return Result<void>::Success();
    }

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

    /** @brief Executes a budgeted cast and validates evidence and owner liveness before reduction. */
    [[nodiscard]] Result<CharacterSweepProbeResult> ProbeStep(auto &impl, const StepQueryContext &query, const StepCast &cast) {
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
        if (input.metrics != nullptr)
            ++input.metrics->snapshot.queries;
        auto probe = input.query.sweep(input.query.context, request);
        if (const auto valid = ValidateTickQueryContinuation(impl, probe); valid.HasError())
            return Result<CharacterSweepProbeResult>::Failure(valid.ErrorValue());
        auto evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return Result<CharacterSweepProbeResult>::Failure(valid.ErrorValue());
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        return Result<CharacterSweepProbeResult>::Success(std::move(evidence));
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
        return Result<bool>::Success(overlap.Value().overlapCount == 0);
    }

    /** @brief Commits a complete checked step; every rejected stage preserves ordinary movement. */
    [[nodiscard]] Result<bool> TryCapsuleStep(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                              const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                              SweepMotionState &motion) {
        const auto rejected = Result<bool>::Success(false);
        if (descriptor.maximumStepHeightMeters <= 0.0F || !MaySnapToGround(command, descriptor.up))
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
        result.gravityVelocityMetersPerSecond = result.grounded ? Math::Vec3{} : motion.gravityVelocity;
        if (steepSupport.has_value()) {
            const float walkableCosine = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F);
            const Math::Vec3 continuation = ClipSlopeMotion(motion.gravityVelocity, steepSupport->normal, descriptor, walkableCosine);
            if (Math::Dot(continuation, descriptor.up) <= GroundNormalTolerance)
                result.gravityVelocityMetersPerSecond = continuation;
        }
        return Result<void>::Success();
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
            if (motion.iteration >= impl.settings.Values().work.maximumMovementIterations)
                return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));
            const auto swept = ResolveCapsuleSweepIteration(impl, result, command, input, airDescriptor, motion, motion.iteration++);
            if (swept.HasError())
                return Result<void>::Failure(swept.ErrorValue());
            if (!swept.Value())
                break;
        }
        result.gravityVelocityMetersPerSecond = motion.gravityVelocity;
        std::optional<CharacterSweepHit> steepSupport;
        if (const auto support = ResolveGrounding(impl, result, command, input, supportDescriptor, motion.position, steepSupport);
            support.HasError())
            return support;
        if (result.grounded)
            result.gravityVelocityMetersPerSecond = {};
        else if (steepSupport) {
            if (descriptor.steepSlopePolicy == CharacterSteepSlopePolicy::Stop)
                result.gravityVelocityMetersPerSecond = {};
            else
                result.gravityVelocityMetersPerSecond = ClipSlopeMotion(motion.gravityVelocity, steepSupport->normal, descriptor,
                                                                        std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.0F));
        }
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
        result.jumpApplied = command.jumpRequested && previous.grounded && descriptor.jumpSpeedMetersPerSecond > 0.0F;
        result.gravityVelocityMetersPerSecond =
            result.jumpApplied ? descriptor.up * descriptor.jumpSpeedMetersPerSecond : previousGravityVelocity;

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

        if (const auto moved = ResolveDesiredDisplacement(impl, result, command, input, descriptor, motion); moved.HasError())
            return Result<CharacterMovementResult>::Failure(moved.ErrorValue());
        std::optional<CharacterSweepHit> steepSupport;
        auto supportDescriptor = descriptor;
        if ((!previous.grounded && previous.sourceTick != 0) || result.jumpApplied)
            supportDescriptor.maximumStepHeightMeters = 0.0F;
        // A complete step already proved its actual contact, support plane and overlap clearance.
        // Ordinary snap must not replace that proof with the capsule's rounded-edge collision normal.
        const bool stepped =
            (static_cast<std::uint16_t>(result.collisions) & static_cast<std::uint16_t>(CharacterCollisionFlags::Step)) != 0;
        if (!stepped) {
            if (const auto grounded = ResolveGrounding(impl, result, command, input, supportDescriptor, motion.position, steepSupport);
                grounded.HasError())
                return Result<CharacterMovementResult>::Failure(grounded.ErrorValue());
        }
        const bool steepPass = steepSupport.has_value();
        if (const auto slide = ResolveSteepSliding(impl, result, command, input, descriptor, motion, steepSupport); slide.HasError())
            return Result<CharacterMovementResult>::Failure(slide.ErrorValue());
        if (!result.grounded && !steepSupport.has_value() && !steepPass) {
            if (const auto air = ResolveAirborneMotion(impl, result, command, input, descriptor, supportDescriptor, motion); air.HasError())
                return Result<CharacterMovementResult>::Failure(air.ErrorValue());
        } else if (result.grounded || descriptor.steepSlopePolicy == CharacterSteepSlopePolicy::Stop) {
            result.gravityVelocityMetersPerSecond = {};
        }
        result.finalPosition = motion.position;
        result.achievedVelocityMetersPerSecond = (motion.position - previous.position) / elapsedSeconds;
        std::ranges::sort(result.contacts.begin(), result.contacts.begin() + result.contactCount, ContactLess);
        return Result<CharacterMovementResult>::Success(std::move(result));
    }
}  // namespace Horo::Character::Detail
