#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <array>
#include <utility>

namespace Horo::Character {
    namespace {
        /** @brief Intersects typed selectors with the movement channel; no trigger or overlap can enter physical reduction. */
        [[nodiscard]] Physics::PhysicsQueryFilter MovementFilter(const Physics::PhysicsQueryChannelId channel,
                                                                 const CharacterCollisionSelectors &selectors) noexcept {
            return {channel,
                    Physics::PhysicsQueryTriggerPolicy::Exclude,
                    selectors.requiredLayer,
                    selectors.requiredProfile,
                    selectors.excludedBody,
                    true};
        }

        /** @brief Builds the common bounded native-query envelope from a Character probe. */
        [[nodiscard]] Physics::PhysicsQueryDescriptor QueryDescriptor(const auto &request, Physics::PhysicsQueryGeometry geometry) {
            return {.world = request.physicsWorld,
                    .sceneGeneration = request.sceneGeneration,
                    .geometry = std::move(geometry),
                    .filter = MovementFilter(request.queryChannel, request.selectors),
                    .collection = Physics::PhysicsQueryCollection::All,
                    .maximumHitCount = MaximumCharacterSweepHits};
        }
    }  // namespace

    /** @copydoc CharacterPhysicsQueryAdapter::CharacterPhysicsQueryAdapter */
    CharacterPhysicsQueryAdapter::CharacterPhysicsQueryAdapter(Physics::PhysicsWorld &world) noexcept : world_(&world) {}

    /** @copydoc CharacterPhysicsQueryAdapter::Context */
    CharacterPhysicsQueryContext CharacterPhysicsQueryAdapter::Context(const CharacterPhysicsQueryExpectations &expected) noexcept {
        return {expected.sceneGeneration,
                expected.characterWorld,
                expected.physicsWorld,
                this,
                Overlap,
                expected.collisionFilterGeneration,
                expected.originGeneration,
                expected.tick,
                expected.physicsSnapshotRevision,
                Sweep};
    }

    /** @copydoc CharacterPhysicsQueryAdapter::Overlap */
    Result<CharacterOverlapProbeResult> CharacterPhysicsQueryAdapter::Overlap(void *context,
                                                                              const CharacterOverlapProbeRequest &request) noexcept {
        auto &adapter = *static_cast<CharacterPhysicsQueryAdapter *>(context);
        std::array<Physics::PhysicsQueryHit, MaximumCharacterSweepHits> hits{};
        const auto descriptor =
            QueryDescriptor(request, Physics::PhysicsCapsuleOverlapQuery{request.capsule, request.position, request.up});
        const auto queried = adapter.world_->Query(descriptor, hits);
        if (queried.HasError())
            return Result<CharacterOverlapProbeResult>::Failure(queried.ErrorValue());
        if (queried.Value().truncated)
            return Result<CharacterOverlapProbeResult>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        CharacterOverlapProbeResult result;
        // Physics sorts by stable Horo identity at an overlap distance tie. Recover against the
        // first positive penetration and re-probe; the Character world owns the iteration budget.
        for (std::uint32_t index{}; index < queried.Value().hitCount; ++index) {
            const auto &hit = hits[index];
            if (hit.penetrationDepthMeters <= 0)
                continue;
            if (!hit.normal)
                return Result<CharacterOverlapProbeResult>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            if (result.overlapCount++ == 0)
                result.recoveryDisplacement = *hit.normal * (hit.penetrationDepthMeters + 1.0e-5F);
        }
        return Result<CharacterOverlapProbeResult>::Success(result);
    }

    /** @copydoc CharacterPhysicsQueryAdapter::Sweep */
    Result<CharacterSweepProbeResult> CharacterPhysicsQueryAdapter::Sweep(void *context,
                                                                          const CharacterSweepProbeRequest &request) noexcept {
        auto &adapter = *static_cast<CharacterPhysicsQueryAdapter *>(context);
        std::array<Physics::PhysicsQueryHit, MaximumCharacterSweepHits> hits{};
        const auto descriptor =
            QueryDescriptor(request, Physics::PhysicsCapsuleSweepQuery{request.capsule, request.position, request.up, request.direction,
                                                                       request.maximumDistanceMeters});
        const auto queried = adapter.world_->Query(descriptor, hits);
        if (queried.HasError())
            return Result<CharacterSweepProbeResult>::Failure(queried.ErrorValue());
        CharacterSweepProbeResult result;
        result.hitCount = queried.Value().hitCount;
        result.truncated = queried.Value().truncated;
        for (std::uint32_t index{}; index < result.hitCount; ++index) {
            const auto &hit = hits[index];
            if (!hit.normal)
                return Result<CharacterSweepProbeResult>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            result.hits[index] = {.body = hit.body,
                                  .shape = hit.shape,
                                  .point = hit.position,
                                  .normal = *hit.normal,
                                  .material = hit.material,
                                  .response = hit.response,
                                  .distanceMeters = hit.distanceMeters,
                                  .subshape = hit.subshape,
                                  .layer = hit.layer,
                                  .profile = hit.profile};
        }
        return Result<CharacterSweepProbeResult>::Success(result);
    }
}  // namespace Horo::Character
