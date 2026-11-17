#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics {
    /** @copydoc PhysicsWorld::ConfigureStateSnapshots */
    Result<std::size_t> PhysicsWorld::ConfigureStateSnapshots(const PhysicsStateCaptureConfiguration &configuration) {
        if (!impl_)
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto prepared = Detail::PhysicsWorldSnapshotState::Prepare(*impl_, configuration);
        if (prepared.HasError())
            return Result<std::size_t>::Failure(prepared.ErrorValue());
        const auto bytes = prepared.Value()->retainedPayloadBytes;
        // All allocation, real native construction, aggregate reservation, and registration have succeeded.
        // The old pool/candidate remains last-good until this same-owner no-fail replacement.
        Detail::RevokePhysicsWorldSnapshots(impl_->snapshots);
        impl_->snapshots = std::move(prepared).Value();
        return Result<std::size_t>::Success(bytes);
    }
}  // namespace Horo::Physics
