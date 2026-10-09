#pragma once

/** @file
 * @brief Target-private canonical sweep evidence, bounded constraints and copied support reduction.
 * No callback may publish a candidate; owner liveness and query budgets remain Character-owned.
 */
#include "CharacterSweepPlanesInternal.h"
#include "CharacterWorldInternal.h"

#include <algorithm>
#include <cmath>
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
        SweepPlanes planes{};
        bool steepConstraint{};
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
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (!IsBlockingSweepHit(hit, descriptor) || Math::Dot(hit.normal, direction) >= 0.0F)
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

    /** @brief Complete blocking normal inventory, separate from the bounded presentation contact prefix. */
    struct SweepNormals final {
        std::array<Math::Vec3, MaximumCharacterSweepHits> active{};
        std::uint32_t count{};
        bool steep{};
    };

    /** @brief Retains every physically eligible nearest constraint before slope response or cone projection. */
    [[nodiscard]] SweepNormals RetainSweepConstraints(CharacterMovementResult &result, const CharacterSweepProbeResult &evidence,
                                                      const CharacterControllerDescriptor &descriptor, const Math::Vec3 direction,
                                                      const float nearest, const float walkableCosine, SweepMotionState &motion) {
        constexpr float DistanceEpsilon = 1.0e-5F;
        SweepNormals normals;
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (!IsBlockingSweepHit(hit, descriptor) || hit.distanceMeters > nearest + DistanceEpsilon ||
                Math::Dot(hit.normal, direction) >= 0.0F)
                continue;
            normals.steep = normals.steep || (Math::Dot(hit.normal, descriptor.up) > 0.0F &&
                                              !IsWalkableGroundNormal(hit.normal, descriptor.up, walkableCosine));
            result.collisions = result.collisions | CollisionFlagForNormal(hit.normal, descriptor.up, walkableCosine);
            RetainSweepContact(result, hit, descriptor);
            if (normals.count < normals.active.size())
                normals.active[normals.count++] = hit.normal;
            if (!motion.planes.Add(hit.normal))
                result.termination = CharacterMovementTermination::ConstraintLimit;
        }
        return normals;
    }

    /** @brief Advances to a blocking skin boundary and projects remaining travel against its canonical normals. */
    void ApplySweepBlock(CharacterMovementResult &result, const CharacterSweepProbeResult &evidence,
                         const CharacterControllerDescriptor &descriptor, const Math::Vec3 direction, const float nearest,
                         const float maximumAscent, SweepMotionState &motion) {
        auto &position = motion.position;
        auto &remaining = motion.remaining;
        // An obstructed gravity slide starts from rest next tick, independently of the
        // retained contact capacity. Never accumulate pressure behind a clipped displacement.
        if (!motion.airbornePass)
            motion.gravityVelocity = {};
        const float travel = std::max(0.0F, nearest - descriptor.skinWidthMeters);
        position += direction * travel;
        remaining -= direction * travel;
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);

        const auto normals = RetainSweepConstraints(result, evidence, descriptor, direction, nearest, walkableCosine, motion);
        for (std::uint32_t index{}; index < normals.count; ++index) {
            remaining = ClipSlopeMotion(remaining, normals.active[index], descriptor, walkableCosine);
            if (motion.airbornePass) {
                const float forbidden = Math::Dot(motion.gravityVelocity, normals.active[index]);
                if (forbidden < 0.0F)
                    motion.gravityVelocity -= normals.active[index] * forbidden;
            }
        }
        motion.steepConstraint = motion.steepConstraint || normals.steep;
        // Use the post-slope intent, but preserve constraints from earlier casts as well as
        // this hit set. A two-plane crease is valid travel, not a reason to stop indefinitely.
        remaining = motion.planes.ClipTravel(remaining, descriptor.up, maximumAscent, motion.steepConstraint);
        if (motion.airbornePass)
            motion.gravityVelocity = motion.planes.Clip(motion.gravityVelocity);
        if (result.termination == CharacterMovementTermination::ConstraintLimit) {
            remaining = {};
            motion.gravityVelocity = {};
        }
    }

    /** @brief Discards unswept travel and continuation at the last checked pose while preserving a committed typed diagnostic. */
    void StopAtMovementLimit(CharacterMovementResult &result, SweepMotionState &motion) noexcept {
        if (result.termination == CharacterMovementTermination::Complete)
            result.termination = CharacterMovementTermination::IterationLimit;
        motion.remaining = {};
        motion.gravityVelocity = {};
        result.gravityVelocityMetersPerSecond = {};
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
        result.platformAttachment.reset();
        result.platformAttachmentChange = CharacterPlatformAttachmentChange::None;
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
        impl.debug.RecordSweep(request, evidence, CharacterDebugProbePurpose::Ground);
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

}  // namespace Horo::Character::Detail
