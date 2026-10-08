#include "Horo/Physics/CharacterClearanceQuery.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <utility>

namespace Horo::Physics {
    /** @copydoc CharacterClearanceQuery::CharacterClearanceQuery */
    CharacterClearanceQuery::CharacterClearanceQuery(PhysicsQueryEventCapability capability,
                                                     const Character::CharacterPhysicsQueryExpectations &expected)
        : capability_(std::move(capability)), expected_(expected) {}

    /** @copydoc CharacterClearanceQuery::Capture */
    Result<CharacterClearanceQuery> CharacterClearanceQuery::Capture(PhysicsQueryEventCapability capability,
                                                                     const Character::CharacterPhysicsQueryExpectations &expected) {
        if (capability.Identity().world != expected.physicsWorld)
            return Result<CharacterClearanceQuery>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
        CharacterClearanceQuery query{std::move(capability), expected};
        if (const auto valid = Character::ValidateCharacterPhysicsQueryContext(query.Context(), expected); valid.HasError())
            return Result<CharacterClearanceQuery>::Failure(valid.ErrorValue());
        return Result<CharacterClearanceQuery>::Success(std::move(query));
    }

    /** @copydoc CharacterClearanceQuery::Context */
    Character::CharacterPhysicsQueryContext CharacterClearanceQuery::Context() & noexcept {
        const Character::CharacterOverlapProbe overlap = [](auto *context,
                                                            const Character::CharacterOverlapProbeRequest &request) noexcept {
            return static_cast<const CharacterClearanceQuery *>(context)->Probe(request);
        };
        return {expected_.sceneGeneration,
                expected_.characterWorld,
                expected_.physicsWorld,
                this,
                overlap,
                expected_.collisionFilterGeneration,
                expected_.originGeneration,
                expected_.tick,
                expected_.physicsSnapshotRevision};
    }

    /** @copydoc CharacterClearanceQuery::Probe */
    Result<Character::CharacterOverlapProbeResult> CharacterClearanceQuery::Probe(
        const Character::CharacterOverlapProbeRequest &request) const noexcept {
        if (request.sceneGeneration != expected_.sceneGeneration || request.characterWorld != expected_.characterWorld ||
            request.physicsWorld != expected_.physicsWorld)
            return Result<Character::CharacterOverlapProbeResult>::Failure(MakeError(Character::CharacterErrors::HandleWorldMismatch));
        if (const auto owner =
                Character::ValidateCharacterControllerHandleOwner(request.controller, expected_.sceneGeneration, expected_.characterWorld);
            owner.HasError())
            return Result<Character::CharacterOverlapProbeResult>::Failure(owner.ErrorValue());
        PhysicsQueryDescriptor descriptor;
        descriptor.world = request.physicsWorld;
        descriptor.sceneGeneration = request.sceneGeneration;
        descriptor.geometry = PhysicsCapsuleOverlapQuery{request.capsule, request.position, request.up};
        descriptor.filter = {request.queryChannel,
                             PhysicsQueryTriggerPolicy::Exclude,
                             request.selectors.requiredLayer,
                             request.selectors.requiredProfile,
                             request.selectors.excludedBody,
                             true};
        descriptor.collection = PhysicsQueryCollection::Any;
        std::array<PhysicsQueryHit, 1> hits{};
        const auto result = capability_.Submit({capability_.Identity(), expected_.physicsSnapshotRevision, descriptor}, hits);
        if (result.HasError())
            return Result<Character::CharacterOverlapProbeResult>::Failure(result.ErrorValue());
        // Overlap contacts tie at zero distance; Physics deterministically ranks blockers first.
        const bool blocked = result.Value().result.hitCount != 0 && hits.front().response == PhysicsQueryResponse::Block;
        return Result<Character::CharacterOverlapProbeResult>::Success({blocked ? 1U : 0U, {}});
    }
}  // namespace Horo::Physics
