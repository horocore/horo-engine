#include "PhysicsStateRestoreControl.h"
#include "PhysicsWorldSnapshotState.h"

#include <exception>
#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsStateRestoreCandidate::PhysicsStateRestoreCandidate */
    PhysicsStateRestoreCandidate::PhysicsStateRestoreCandidate(std::shared_ptr<Impl> impl, std::uint64_t serial) noexcept
        : impl_(std::move(impl)), serial_(serial) {}

    /** @copydoc PhysicsStateRestoreCandidate::PhysicsStateRestoreCandidate */
    PhysicsStateRestoreCandidate::PhysicsStateRestoreCandidate(PhysicsStateRestoreCandidate &&other) noexcept
        : impl_(std::move(other.impl_)), serial_(std::exchange(other.serial_, 0)) {}

    /** @copydoc PhysicsStateRestoreCandidate::operator= */
    PhysicsStateRestoreCandidate &PhysicsStateRestoreCandidate::operator=(PhysicsStateRestoreCandidate &&other) noexcept {
        if (this != &other) {
            Release();
            impl_ = std::move(other.impl_);
            serial_ = std::exchange(other.serial_, 0);
        }
        return *this;
    }

    /** @copydoc PhysicsStateRestoreCandidate::Release */
    void PhysicsStateRestoreCandidate::Release() noexcept {
        auto control = std::move(impl_);
        const auto serial = std::exchange(serial_, 0);
        if (!control || serial == 0)
            return;
        control->abandonedSerial.store(serial, std::memory_order_release);
        // Owner acquires false before reuse. This is the last mutable control access
        // by this handle.
        control->activeHandle.store(false, std::memory_order_release);
    }

    /** @copydoc PhysicsStateRestoreCandidate::~PhysicsStateRestoreCandidate */
    PhysicsStateRestoreCandidate::~PhysicsStateRestoreCandidate() {
        Release();
    }

    /** @copydoc PhysicsStateRestoreCandidate::ValidatePublication */
    Result<void> PhysicsStateRestoreCandidate::ValidatePublication() const {
        if (!impl_ || serial_ == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        auto owner = impl_->owner.lock();
        if (!owner || owner->closed.load(std::memory_order_acquire))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
        return owner->ValidatePendingRestore(serial_);
    }

    /** @copydoc PhysicsStateRestoreCandidate::Publish */
    void PhysicsStateRestoreCandidate::Publish() noexcept {
        if (!impl_ || serial_ == 0 || impl_->ownerThread != std::this_thread::get_id())
            std::terminate();
        auto owner = impl_->owner.lock();
        if (!owner || owner->closed.load(std::memory_order_acquire))
            std::terminate();
        owner->PublishPendingRestore(serial_);
        serial_ = 0;
        impl_->activeHandle.store(false, std::memory_order_release);
        impl_.reset();
    }
}  // namespace Horo::Physics
