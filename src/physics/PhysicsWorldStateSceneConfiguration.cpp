#include "PhysicsWorldSnapshotState.h"
#include "PhysicsWorldStateRestartAccess.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldStateRestartAccess::ConfigurationOwner */
    Result<std::weak_ptr<PhysicsWorldSnapshotState>> PhysicsWorldStateRestartAccess::ConfigurationOwner(const PhysicsWorld &world) {
        using Outcome = Result<std::weak_ptr<PhysicsWorldSnapshotState>>;
        if (!world.impl_)
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (world.impl_->runtime->ownerThread != std::this_thread::get_id())
            return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!world.impl_->snapshots)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        return Outcome::Success(world.impl_->snapshots);
    }

    /** @copydoc PhysicsWorldStateRestartAccess::ConfigurationPayloadBytes */
    Result<std::size_t> PhysicsWorldStateRestartAccess::ConfigurationPayloadBytes(const PhysicsWorld &world) {
        auto actual = ConfigurationOwner(world);
        if (actual.HasError())
            return Result<std::size_t>::Failure(actual.ErrorValue());
        const auto owner = actual.Value().lock();
        if (!owner)
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
        return Result<std::size_t>::Success(owner->retainedPayloadBytes);
    }

    /** @copydoc PhysicsWorldStateRestartAccess::ValidateConfigured */
    Result<void> PhysicsWorldStateRestartAccess::ValidateConfigured(const PhysicsWorld &world,
                                                                    const std::weak_ptr<PhysicsWorldSnapshotState> &expected,
                                                                    std::uint64_t publicationRevision) {
        auto actual = ConfigurationOwner(world);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        const auto owner = expected.lock();
        if (!owner || owner != actual.Value().lock())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (publicationRevision && world.impl_->publication.Snapshot().publicationRevision != publicationRevision)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return owner->CheckCurrentWorld();
    }

    /** @copydoc PhysicsWorldStateRestartAccess::CaptureConfigured */
    Result<PhysicsStateSnapshotLease> PhysicsWorldStateRestartAccess::CaptureConfigured(
        const PhysicsWorld &world, const std::weak_ptr<PhysicsWorldSnapshotState> &expected) {
        if (auto exact = ValidateConfigured(world, expected, 0); exact.HasError())
            return Result<PhysicsStateSnapshotLease>::Failure(exact.ErrorValue());
        return world.CaptureState();
    }
}  // namespace Horo::Physics::Detail
