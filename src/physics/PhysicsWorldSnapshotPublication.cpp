#include "PhysicsWorldSnapshotState.h"

#include <limits>

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::CheckJoinedOwner */
    Result<void> PhysicsWorldSnapshotState::CheckJoinedOwner() const {
        if (ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (closed.load(std::memory_order_acquire) || !world)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
        if (world->state != PhysicsWorldState::ActiveSolver || world->runtime->state != PhysicsRuntimeState::Ready)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (world->stepping || world->commandCount != 0 || world->queryBatch.pending)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::CheckPendingSourcePublication */
    Result<void> PhysicsWorldSnapshotState::CheckPendingSourcePublication(std::uint64_t serial) const {
        if (!pendingRestore || pendingRestoreSerial != serial || !pendingSnapshot.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (restoreCancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
        if (world->publication.Snapshot().publicationRevision != pendingPublicationRevision)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (auto revision = world->CheckPublicationRevisionCapacity(); revision.HasError())
            return revision;
        if (world->statistics.completedTicks == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::CheckPendingNativeTopology */
    Result<void> PhysicsWorldSnapshotState::CheckPendingNativeTopology() const {
        if (auto native = workspace->ValidateRestoredOwnerSwap(world->native, pendingNative); native.HasError())
            return native;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::ValidatePendingRestore */
    Result<void> PhysicsWorldSnapshotState::ValidatePendingRestore(std::uint64_t serial) {
        // Revalidation invalidates an older arm first; failure can never leave a publishable stale claim.
        validatedRestoreSerial = 0;
        if (auto current = CheckCurrentWorld(); current.HasError())
            return current;
        if (auto source = CheckPendingSourcePublication(serial); source.HasError())
            return source;
        if (auto native = CheckPendingNativeTopology(); native.HasError())
            return native;
        validatedPublicationRevision = pendingPublicationRevision;
        validatedRestoreSerial = serial;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::PublishPendingRestore */
    void PhysicsWorldSnapshotState::PublishPendingRestore(std::uint64_t serial) noexcept {
        // This method consumes the successful same-owner-fence validation claim. A foreign token change after
        // that claim is too late; it must not turn the no-fail publication phase into an ordinary error/fatal path.
        if (!world || serial == 0 || serial != validatedRestoreSerial || serial != pendingRestoreSerial ||
            world->publication.Snapshot().publicationRevision != validatedPublicationRevision)
            std::terminate();
        const auto restored = pendingSnapshot->World();
        workspace->AdoptRestoredOwner(world->native, pendingNative);
        world->native = pendingNative;
        world->InvalidateQueryEventCapabilities();
        world->queryEvents.events.SwapState(*stagedEvents);
        world->queryBatch.tick = 0;
        world->queryBatch.admissions = 0;
        world->statistics.eventDepth = static_cast<std::uint32_t>(restored.publishedEvents.size());
        world->statistics.maximumEventDepth = std::max(world->statistics.maximumEventDepth, world->statistics.eventDepth);
        ++world->statistics.completedTicks;
        world->publication.Commit(restored.completedTick, restored.appliedCommands,
                                  {world->statistics.eventDepth, restored.droppedEventCount, false});
        pendingRestore = false;
        pendingRestoreSerial = validatedRestoreSerial = 0;
        pendingNative = {};
        pendingSnapshot = {};
        restoreCancellation = {};
    }
}  // namespace Horo::Physics::Detail
