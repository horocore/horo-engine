#include "CharacterWorldInternal.h"
#include "PhysicsWorldInternal.h"
#include "internal/CharacterScenePlacement.h"

#include <algorithm>

namespace Horo::Character::Detail {
    namespace {
        bool NoTickWork(const CharacterFastPathSnapshot &work) noexcept {
            return work.retainedContacts == 0 && work.queuedHits == 0 && work.queuedEvents == 0 && work.stagedImpulses == 0 &&
                   work.scratchBytesUsed == 0;
        }

        bool NoCommandHistory(const auto &owner) noexcept {
            return owner.closedTick.load() == 0 && owner.pendingCommands.load() == 0 && owner.admittedCommands.load() == 0 &&
                   owner.completedTicks.load() == 0;
        }

        bool NeverOwnedControllers(const auto &owner) noexcept {
            const auto lock = owner.synchronization.LockRegistry();
            return owner.controllers.Statistics().active == 0 && std::ranges::all_of(owner.controllerGenerations, [](auto generation) {
                return generation == 0;
            });
        }

        Result<void> EmptyUnticked(const auto &owner) {
            if (auto lane = RequireOwnerThread(owner.ownerThread); lane.HasError())
                return lane;
            if (owner.state.load() != CharacterWorldState::Active || owner.ticking.load() || owner.placementActive ||
                owner.shutdownRequested)
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            if (!NoCommandHistory(owner) || !NeverOwnedControllers(owner) || !NoTickWork(owner.fastPath.Snapshot()))
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            return Result<void>::Success();
        }

        bool JoinedPhysics(const auto &owner) noexcept {
            return owner.state == Physics::PhysicsWorldState::ActiveSolver && !owner.stepping && owner.commandCount == 0 &&
                   !owner.queryBatch.pending;
        }

        Result<void> PairEvidence(const auto &character, const auto &physics, Runtime::SceneRuntimeId scene) {
            if (physics.runtime->ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (!JoinedPhysics(physics))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            if (!scene.IsValid() || character.descriptor.sceneGeneration != scene.value ||
                character.descriptor.physicsWorld != physics.identity)
                return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
            if (physics.querySceneGeneration && physics.querySceneGeneration != scene.value)
                return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
            return Result<void>::Success();
        }

        bool CompletePublication(const Physics::PhysicsPublishedTick &publication) noexcept {
            return publication.completedTick && publication.publicationRevision && publication.transformTick == publication.completedTick &&
                   publication.queryTick == publication.completedTick && publication.eventTick == publication.completedTick;
        }
    }  // namespace

    /** @copydoc CharacterScenePlacement::ValidateEmptyPendingContinuation */
    Result<void> CharacterScenePlacement::ValidateEmptyPendingContinuation(CharacterWorld &character, Physics::PhysicsWorld &physics,
                                                                           Runtime::SceneRuntimeId scene) {
        auto &owner = *character.impl_;
        if (auto empty = EmptyUnticked(owner); empty.HasError())
            return empty;
        if (auto pair = PairEvidence(owner, *physics.impl_, scene); pair.HasError())
            return pair;
        const auto lock = owner.synchronization.LockPublication();
        if (owner.published.completedTick || owner.published.publicationRevision || physics.PublishedTick().completedTick)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc CharacterScenePlacement::AdoptEmptyPendingContinuation */
    Result<void> CharacterScenePlacement::AdoptEmptyPendingContinuation(CharacterWorld &character, Physics::PhysicsWorld &physics,
                                                                        Runtime::SceneRuntimeId scene) {
        auto &owner = *character.impl_;
        if (auto empty = EmptyUnticked(owner); empty.HasError())
            return empty;
        if (auto pair = PairEvidence(owner, *physics.impl_, scene); pair.HasError())
            return pair;
        const auto publication = physics.PublishedTick();
        if (!CompletePublication(publication))
            return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        const auto lock = owner.synchronization.LockPublication();
        if (owner.published.completedTick || owner.published.publicationRevision)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        physics.impl_->querySceneGeneration = scene.value;
        owner.descriptor.physicsSnapshotRevision = publication.publicationRevision;
        owner.published = {.completedTick = publication.completedTick, .publicationRevision = 1, .appliedCommands = 0};
        owner.closedTick.store(publication.completedTick);
        return Result<void>::Success();
    }
}  // namespace Horo::Character::Detail
