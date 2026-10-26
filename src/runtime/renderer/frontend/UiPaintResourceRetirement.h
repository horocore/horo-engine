#pragma once

/** @file UiPaintResourceRetirement.h
 * @brief Frontend-private UI retirement operations borrowing the canonical resource registry. */
#include "RenderResourceRegistry.h"

namespace Horo::Render::Detail {
    /** @brief Stateless UI ownership policy; registry storage, generation checks and pins remain authoritative.
     * @details Borrow only on the frontend owner thread outside an active frame. The registry outlives this view. */
    class UiPaintResourceRetirement final {
    public:
        /** @brief Borrows the existing registry without allocating or changing resource state. */
        explicit UiPaintResourceRetirement(RenderResourceRegistry &registry) noexcept : registry_(registry) {}

        /** @brief Attaches one admitted UI owner before its exact pending creation is published to a caller. */
        void BindOwner(const ResourceReservation &reservation, const std::shared_ptr<UiPaintOwnerRetirement> &owner) const noexcept;
        /** @brief Finds requested pending ownership without copying or constructing operation diagnostics. */
        [[nodiscard]] std::optional<ResourceReservation> PendingReservation(RenderResourceIdentity identity) const noexcept;
        /** @brief Marks one exact requested UI generation retiring; existing pins still gate native destruction. */
        void Retire(RenderResourceIdentity identity, ResourceOperationId operation) const noexcept;
        /** @brief Examines at most the configured retirement budget using a persistent cursor; pending uploads drain separately. */
        void Sweep() const noexcept;

    private:
        RenderResourceRegistry &registry_;
    };
}  // namespace Horo::Render::Detail
