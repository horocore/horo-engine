#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics {
    namespace {
        Result<std::shared_ptr<Detail::PhysicsWorldSnapshotState>> StateOwner(auto &owner) {
            using Outcome = Result<std::shared_ptr<Detail::PhysicsWorldSnapshotState>>;
            if (owner.runtime->ownerThread != std::this_thread::get_id())
                return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            if (!owner.snapshots)
                return Outcome::Failure(MakeError(owner.runtime->mode == PhysicsRuntimeMode::Null ? PhysicsErrors::OperationUnsupported
                                                                                                  : PhysicsErrors::CapabilityUnavailable));
            return Outcome::Success(owner.snapshots);
        }
    }  // namespace

    /** @copydoc PhysicsWorld::CaptureState */
    Result<PhysicsStateSnapshotLease> PhysicsWorld::CaptureState() const {
        if (!impl_)
            return Result<PhysicsStateSnapshotLease>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto owner = StateOwner(*impl_);
        if (owner.HasError())
            return Result<PhysicsStateSnapshotLease>::Failure(owner.ErrorValue());
        return owner.Value()->Capture();
    }

    /** @copydoc PhysicsWorld::DecodeStateSnapshot */
    Result<PhysicsStateSnapshotLease> PhysicsWorld::DecodeStateSnapshot(std::span<const std::byte> bytes) const {
        if (!impl_)
            return Result<PhysicsStateSnapshotLease>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto owner = StateOwner(*impl_);
        if (owner.HasError())
            return Result<PhysicsStateSnapshotLease>::Failure(owner.ErrorValue());
        return owner.Value()->Decode(bytes);
    }

    /** @copydoc PhysicsWorld::PrepareStateRestore */
    Result<PhysicsStateRestoreCandidate> PhysicsWorld::PrepareStateRestore(PhysicsStateSnapshotLease snapshot,
                                                                           const CancellationToken &cancellation) {
        if (!impl_)
            return Result<PhysicsStateRestoreCandidate>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto owner = StateOwner(*impl_);
        if (owner.HasError())
            return Result<PhysicsStateRestoreCandidate>::Failure(owner.ErrorValue());
        return owner.Value()->PrepareRestore(std::move(snapshot), cancellation);
    }
}  // namespace Horo::Physics
