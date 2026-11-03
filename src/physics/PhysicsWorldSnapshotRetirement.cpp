#include "PhysicsStateRestoreControl.h"
#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::CollectAbandonedRestore */
    void PhysicsWorldSnapshotState::CollectAbandonedRestore() noexcept {
        if (!pendingRestore || !restoreControl || restoreControl->activeHandle.load(std::memory_order_acquire))
            return;
        if (restoreControl->abandonedSerial.load(std::memory_order_acquire) != pendingRestoreSerial)
            return;
        pendingRestore = false;
        pendingRestoreSerial = 0;
        validatedRestoreSerial = 0;
        pendingNative = {};
        restoreCancellation = {};
        pendingSnapshot = {};
    }

    /** @copydoc PhysicsWorldSnapshotState::Close */
    void PhysicsWorldSnapshotState::Close() noexcept {
        if (closed.load(std::memory_order_acquire))
            return;
        if (ownerThread != std::this_thread::get_id())
            std::terminate();
        if (closed.exchange(true, std::memory_order_acq_rel))
            return;
        if (workspace) {
            workspace.reset();
        }
        if (candidateRegistered) {
            if (!world || world->runtime->nativeStateCandidates == 0)
                std::terminate();
            --world->runtime->nativeStateCandidates;
            candidateRegistered = false;
        }
        stagedEvents.reset();
        pendingRestore = false;
        pendingRestoreSerial = 0;
        validatedRestoreSerial = 0;
        pendingNative = {};
        restoreCancellation = {};
        pendingSnapshot = {};
        world = nullptr;
        slots.clear();
        resources.reset();
    }

    /** @copydoc RevokePhysicsWorldSnapshots */
    void RevokePhysicsWorldSnapshots(std::shared_ptr<PhysicsWorldSnapshotState> &state) noexcept {
        if (!state)
            return;
        state->Close();
        state.reset();
    }
}  // namespace Horo::Physics::Detail
