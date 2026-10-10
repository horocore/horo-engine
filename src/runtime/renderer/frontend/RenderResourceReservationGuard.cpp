#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "RenderResourceRegistry.h"

#include <cassert>

namespace Horo::Render::Detail {
    /** @copydoc ResourceReservationGuard::ResourceReservationGuard */
    ResourceReservationGuard::ResourceReservationGuard(RenderResourceRegistry &registry, const RenderResourceClass resourceClass,
                                                       const ResourceReservation reservation) noexcept
        : registry_(registry), resourceClass_(resourceClass), reservation_(reservation) {}

    /** @copydoc ResourceReservationGuard::~ResourceReservationGuard */
    ResourceReservationGuard::~ResourceReservationGuard() noexcept {
        if (!armed_)
            return;
        if (budget_ != nullptr)
            static_cast<void>(budget_->Cancel(memory_));
        registry_.RollbackPending(resourceClass_, reservation_);
        static_cast<void>(registry_.DrainRetirements());
    }

    /** @copydoc ResourceReservationGuard::OwnMemory */
    void ResourceReservationGuard::OwnMemory(RenderMemoryBudget &budget, const RenderMemoryReservationId reservation) noexcept {
        assert(budget_ == nullptr && reservation.IsValid());
        budget_ = &budget;
        memory_ = reservation;
    }

    /** @copydoc ResourceReservationGuard::Commit */
    void ResourceReservationGuard::Commit() noexcept {
        armed_ = false;
    }

}  // namespace Horo::Render::Detail
