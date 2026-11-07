#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsWorldSnapshotState.h"
#include "PhysicsWorldStateEvents.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::CheckCurrentWorld */
    Result<void> PhysicsWorldSnapshotState::CheckCurrentWorld() const {
        if (auto joined = CheckJoinedOwner(); joined.HasError())
            return joined;
        if (!workspace || !resources || !world->runtime->stateBuild)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
        auto floatingPoint = ReadCanonicalPhysicsStateFpControl();
        if (floatingPoint.HasError())
            return Result<void>::Failure(floatingPoint.ErrorValue());
        if (floatingPoint.Value() != compatibility.floatingPoint)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        const auto current = workspace->TopologyFingerprint(world->native);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value() != compatibility.topology || world->settings.Identity() != compatibility.settings)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::ValidateSnapshot */
    Result<void> PhysicsWorldSnapshotState::ValidateSnapshot(const PhysicsStateSnapshot &snapshot) {
        if (snapshot.Compatibility() != compatibility)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        if (!std::ranges::equal(snapshot.ConstructionBytes(), workspace->ConstructionBytes()))
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        const auto source = snapshot.World();
        if (auto state = workspace->ValidateState(snapshot.Solver(), source.continuousCollision); state.HasError())
            return state;
        return PreparePhysicsStateEvents(*workspace, source, eventPairScratch, eventScratch, *stagedEvents);
    }
}  // namespace Horo::Physics::Detail
