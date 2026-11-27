#include "PhysicsStateSnapshotInternal.h"

#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsStateSnapshot::PhysicsStateSnapshot */
    PhysicsStateSnapshot::PhysicsStateSnapshot(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PhysicsStateSnapshot::~PhysicsStateSnapshot */
    PhysicsStateSnapshot::~PhysicsStateSnapshot() = default;

    /** @copydoc PhysicsStateSnapshot::Compatibility */
    const PhysicsStateCompatibility &PhysicsStateSnapshot::Compatibility() const noexcept {
        return impl_->compatibility;
    }

    /** @copydoc PhysicsStateSnapshot::Solver */
    PhysicsStateSolverView PhysicsStateSnapshot::Solver() const noexcept {
        return impl_->solver->used;
    }

    /** @copydoc PhysicsStateSnapshot::World */
    PhysicsStateWorldView PhysicsStateSnapshot::World() const noexcept {
        return impl_->world;
    }

    /** @copydoc PhysicsStateSnapshot::Bytes */
    std::span<const std::byte> PhysicsStateSnapshot::Bytes() const noexcept {
        return std::span<const std::byte>{impl_->wire}.first(impl_->usedWireBytes);
    }

    /** @copydoc PhysicsStateSnapshot::ConstructionBytes */
    std::span<const std::byte> PhysicsStateSnapshot::ConstructionBytes() const noexcept {
        return impl_->construction;
    }

    /** @copydoc PhysicsStateSnapshot::RetainedPayloadBytes */
    std::size_t PhysicsStateSnapshot::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + sizeof(Impl) + impl_->solver->RetainedPayloadBytes() +
               impl_->pairs.capacity() * sizeof(PhysicsStateEventPair) + impl_->events.capacity() * sizeof(PhysicsStatePublishedEvent) +
               impl_->wire.capacity();
    }
}  // namespace Horo::Physics
