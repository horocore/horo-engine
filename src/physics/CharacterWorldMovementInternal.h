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
                                        left.relativeVelocityMetersPerSecond.z};
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
                                         right.relativeVelocityMetersPerSecond.z};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        if (left.material.has_value() != right.material.has_value())
            return !left.material.has_value();
        if (!left.material.has_value())
            return false;
        const auto leftSlot = left.material->slot.Value();
        const auto rightSlot = right.material->slot.Value();
        return std::tie(left.material->asset.Bytes(), left.material->assetGeneration, leftSlot) <
               std::tie(right.material->asset.Bytes(), right.material->assetGeneration, rightSlot);
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
                                        left.point.z};
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
                                         right.point.z};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
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
            return contact.body == hit.body && contact.shape == hit.shape && contact.normal == hit.normal;
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
        contact.penetrationDepthMeters = std::max(0.0F, descriptor.skinWidthMeters - hit.distanceMeters);
    }

    struct SweepBlockSelection final {
        float nearest{};
        bool blocked{};
    };

    struct SweepMotionState final {
        Math::Vec3 position{};
        Math::Vec3 remaining{};
    };

    /** @brief Selects the canonical nearest blocking hit from one sorted response prefix. */
    [[nodiscard]] SweepBlockSelection SelectNearestSweepBlock(const CharacterSweepProbeResult &evidence, const Math::Vec3 direction,
                                                              const float distance) noexcept {
        SweepBlockSelection selection{distance};
        constexpr float NormalEpsilon = 1.0e-5F;
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (hit.response != Physics::PhysicsQueryResponse::Block || Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                continue;
            // The active prefix is sorted by distance before this reducer runs, so the first valid
            // blocking hit is the nearest one and no later evidence can change the selection.
            selection.nearest = hit.distanceMeters;
            selection.blocked = true;
            break;
        }
        return selection;
    }

    /** @brief Advances to a blocking skin boundary and projects remaining travel against its canonical normals. */
    void ApplySweepBlock(CharacterMovementResult &result, const CharacterSweepProbeResult &evidence,
                         const CharacterControllerDescriptor &descriptor, const Math::Vec3 direction, const float nearest,
                         Math::Vec3 &position, Math::Vec3 &remaining) {
        constexpr float DistanceEpsilon = 1.0e-5F;
        constexpr float NormalEpsilon = 1.0e-5F;
        const float travel = std::max(0.0F, nearest - descriptor.skinWidthMeters);
        position += direction * travel;
        remaining -= direction * travel;
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);

        std::array<Math::Vec3, MaximumCharacterSweepHits> activeNormals{};
        std::uint32_t activeNormalCount{};
        for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
            const auto &hit = evidence.hits[index];
            if (hit.response != Physics::PhysicsQueryResponse::Block || hit.distanceMeters > nearest + DistanceEpsilon ||
                Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                continue;
            result.collisions = result.collisions | CollisionFlagForNormal(hit.normal, descriptor.up, walkableCosine);
            RetainSweepContact(result, hit, descriptor);
            if (activeNormalCount < activeNormals.size())
                activeNormals[activeNormalCount++] = hit.normal;
        }

        const float beforeProjection = Math::LengthSquared(remaining);
        for (std::uint32_t index{}; index < activeNormalCount; ++index) {
            const float intoSurface = Math::Dot(remaining, activeNormals[index]);
            if (intoSurface < 0.0F)
                remaining -= activeNormals[index] * intoSurface;
        }
        const float afterProjection = Math::LengthSquared(remaining);
        // Adapter normals are unit-validated within a tolerance. Preserve the incoming travel
        // magnitude if that tolerance and repeated projections produce finite round-off growth.
        if (std::isfinite(beforeProjection) && std::isfinite(afterProjection) && afterProjection > beforeProjection &&
            afterProjection > 0.0F)
            remaining *= std::sqrt(beforeProjection / afterProjection);
    }

    /** @brief Clears all optional support evidence while retaining the explicit up basis. */
    void ClearGroundEvidence(CharacterMovementResult &result, const Math::Vec3 up) noexcept {
        result.grounded = false;
        result.groundSlopeDegrees = 0.0F;
        result.groundNormal = up;
        result.groundMaterial.reset();
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
            if (hit.response != Physics::PhysicsQueryResponse::Block || hit.distanceMeters > snapDistance + GroundDistanceTolerance ||
                !IsWalkableGroundNormal(hit.normal, descriptor.up, walkableCosine))
                continue;
            return hit;
        }
        return std::nullopt;
    }

    /** @brief Resolves bounded floor classification and downward snap after ordinary movement. */
    [[nodiscard]] Result<void> ResolveGrounding(auto &impl, CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                const CharacterFixedTickInput &input, const CharacterControllerDescriptor &descriptor,
                                                Math::Vec3 &position) {
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
                                                 impl.settings.Values().work.maximumMovementIterations};
        auto probe = input.query.sweep(input.query.context, request);
        if (impl.state.load() != CharacterWorldState::Active)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (probe.HasError())
            return Result<void>::Failure(probe.ErrorValue());
        CharacterSweepProbeResult evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return valid;
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        const auto support = SelectGroundHit(evidence, descriptor);
        if (!support.has_value())
            return Result<void>::Success();

        const float snap = std::max(0.0F, support->distanceMeters - descriptor.skinWidthMeters);
        position += down * snap;
        result.grounded = true;
        result.groundSlopeDegrees = GroundSlopeDegrees(support->normal, descriptor.up);
        result.groundNormal = support->normal;
        result.groundMaterial = support->material.value_or(descriptor.defaultMaterial);
        result.groundBody = support->body;
        result.groundShape = support->shape;
        result.groundDistanceMeters = support->distanceMeters - snap;
        result.groundRelativeVelocityMetersPerSecond = support->relativeVelocityMetersPerSecond;
        result.collisions = result.collisions | CharacterCollisionFlags::Ground;
        result.groundingRevalidationRequired = false;
        RetainSweepContact(result, *support, descriptor);
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
                                                 iteration};
        auto probe = input.query.sweep(input.query.context, request);
        if (impl.state.load() != CharacterWorldState::Active)
            return Result<bool>::Failure(MakeError(CharacterErrors::InvalidState));
        if (probe.HasError())
            return Result<bool>::Failure(probe.ErrorValue());
        CharacterSweepProbeResult evidence = std::move(probe).Value();
        if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);
        const auto selection = SelectNearestSweepBlock(evidence, direction, distance);
        if (!selection.blocked) {
            motion.position += motion.remaining;
            motion.remaining = {};
            return Result<bool>::Success(false);
        }
        ApplySweepBlock(result, evidence, descriptor, direction, selection.nearest, motion.position, motion.remaining);
        return Result<bool>::Success(true);
    }

    /**
     * @brief Resolves one desired displacement through bounded Horo capsule sweeps and iterative slide.
     *
     * The callback only supplies copied evidence. Character owns ordering, skin-width advancement,
     * contact retention and projection against the canonical normal prefix, so native traversal order
     * cannot change the resulting motion.
     */
    [[nodiscard]] Result<CharacterMovementResult> BuildCapsuleSweepMovementResult(const auto &impl, const CharacterMovementRequest &command,
                                                                                  const CharacterTransformPublication &previous,
                                                                                  const CharacterFixedTickInput &input,
                                                                                  const CharacterControllerDescriptor &descriptor) {
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
        SweepMotionState motion{previous.position, command.desiredVelocityMetersPerSecond.value_or(Math::Vec3{}) * elapsedSeconds};
        if (!Math::IsFinite(motion.remaining))
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character desired displacement is not finite."));

        for (std::uint32_t iteration{}; iteration < impl.settings.Values().work.maximumMovementIterations; ++iteration) {
            const auto resolved = ResolveCapsuleSweepIteration(impl, result, command, input, descriptor, motion, iteration);
            if (resolved.HasError())
                return Result<CharacterMovementResult>::Failure(resolved.ErrorValue());
            if (!resolved.Value())
                break;
        }
        if (const auto grounded = ResolveGrounding(impl, result, command, input, descriptor, motion.position); grounded.HasError())
            return Result<CharacterMovementResult>::Failure(grounded.ErrorValue());
        result.finalPosition = motion.position;
        result.achievedVelocityMetersPerSecond = (motion.position - previous.position) / elapsedSeconds;
        std::ranges::sort(result.contacts.begin(), result.contacts.begin() + result.contactCount, ContactLess);
        return Result<CharacterMovementResult>::Success(std::move(result));
    }
}  // namespace Horo::Character::Detail
