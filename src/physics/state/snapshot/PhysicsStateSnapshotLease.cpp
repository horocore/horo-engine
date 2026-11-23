#include "PhysicsStateSnapshotSlot.h"

#include <exception>
#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsStateSnapshotLease::PhysicsStateSnapshotLease */
    PhysicsStateSnapshotLease::PhysicsStateSnapshotLease(const PhysicsStateSnapshotLease &other) noexcept : slot_(other.slot_) {
        if (!slot_)
            return;
        const auto previous = slot_->readers.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0 || previous >= Detail::PhysicsStateSnapshotSlot::Writing - 1)
            std::terminate();
    }

    /** @brief Releases the old reader only after the replacement has safely
     * retained its existing immutable publication. */
    PhysicsStateSnapshotLease &PhysicsStateSnapshotLease::operator=(const PhysicsStateSnapshotLease &other) noexcept {
        if (this != &other) {
            PhysicsStateSnapshotLease retained{other};
            std::swap(slot_, retained.slot_);
        }
        return *this;
    }

    /** @brief Transfers one existing reader without adding or dropping the
     * transferred reference. */
    PhysicsStateSnapshotLease &PhysicsStateSnapshotLease::operator=(PhysicsStateSnapshotLease &&other) noexcept {
        if (this != &other) {
            Release();
            slot_ = std::move(other.slot_);
        }
        return *this;
    }

    /** @copydoc PhysicsStateSnapshotLease::Release */
    void PhysicsStateSnapshotLease::Release() noexcept {
        auto retained = std::move(slot_);
        if (!retained)
            return;
        const auto previous = retained->readers.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 0 || previous == Detail::PhysicsStateSnapshotSlot::Writing)
            std::terminate();
    }

    PhysicsStateSnapshotLease::~PhysicsStateSnapshotLease() {
        Release();
    }

    /** @copydoc PhysicsStateSnapshotLease::Get */
    const PhysicsStateSnapshot *PhysicsStateSnapshotLease::Get() const noexcept {
        return slot_ ? slot_->snapshot.get() : nullptr;
    }
}  // namespace Horo::Physics
