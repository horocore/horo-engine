#include "PhysicsStateRestoreControl.h"
#include "PhysicsWorldSnapshotState.h"

#include <limits>

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::CheckRestoreAdmission */
    Result<void> PhysicsWorldSnapshotState::CheckRestoreAdmission(const PhysicsStateSnapshotLease &snapshot,
                                                                  const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
        if (!snapshot.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (pendingRestore || !restoreControl || restoreControl->activeHandle.load(std::memory_order_acquire))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (nextRestoreSerial == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::RegisterRestore */
    PhysicsStateRestoreCandidate PhysicsWorldSnapshotState::RegisterRestore(PhysicsStateSnapshotLease snapshot,
                                                                            const CancellationToken &cancellation,
                                                                            CanonicalWorldHandle native) noexcept {
        pendingRestoreSerial = nextRestoreSerial++;
        pendingPublicationRevision = world->publication.Snapshot().publicationRevision;
        pendingNative = native;
        pendingSnapshot = std::move(snapshot);
        restoreCancellation = cancellation;
        validatedRestoreSerial = 0;
        pendingRestore = true;
        restoreControl->abandonedSerial.store(0, std::memory_order_relaxed);
        restoreControl->activeHandle.store(true, std::memory_order_release);
        return PhysicsStateRestoreCandidate{restoreControl, pendingRestoreSerial};
    }

    /** @copydoc PhysicsWorldSnapshotState::PrepareRestore */
    Result<PhysicsStateRestoreCandidate> PhysicsWorldSnapshotState::PrepareRestore(PhysicsStateSnapshotLease snapshot,
                                                                                   const CancellationToken &cancellation) {
        using Outcome = Result<PhysicsStateRestoreCandidate>;
        if (auto current = CheckCurrentWorld(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        CollectAbandonedRestore();
        if (auto admitted = CheckRestoreAdmission(snapshot, cancellation); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        if (auto validated = ValidateSnapshot(*snapshot.Get()); validated.HasError())
            return Outcome::Failure(validated.ErrorValue());
        auto restored = workspace->Restore(world->native, snapshot->Solver(), snapshot->World().continuousCollision);
        if (restored.HasError())
            return Outcome::Failure(restored.ErrorValue());
        // All writes so far were to the retained unpublished native owner and staging projection.
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(PhysicsErrors::StateCancelled));
        return Outcome::Success(RegisterRestore(std::move(snapshot), cancellation, restored.Value()));
    }
}  // namespace Horo::Physics::Detail
