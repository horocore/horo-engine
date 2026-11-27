#pragma once

/** @file AiDecisionTaskReservation.h
 * @brief Move-only owner-issued task namespace and cancellation scope for one decision instance.
 */

#include "Horo/AI/BlackboardInstance.h"
#include "Horo/Foundation/CancellationToken.h"

#include <thread>

namespace Horo::AI {
    class AiSceneRuntime;

    /** @brief Exclusive logical task-slot reservation issued by the actual Scene AI allocator.
     * @details Metadata is inspectable but cannot issue another reservation. Moving transfers the sole scope owner;
     * destruction cancels its tokens without returning indices to the allocator. The source agent's cancellation
     * ancestry also revokes the scope at disable, restore or Scene retirement. No Scene pointer or callback is retained.
     */
    class AiDecisionTaskReservation final {
    public:
        /** @brief Transfers the cancellation owner and invalidates the moved-from reservation.
         * @param other Reservation whose sole scope ownership is transferred.
         */
        AiDecisionTaskReservation(AiDecisionTaskReservation &&other) noexcept;
        /** @brief Cancels any previous scope, then transfers ownership.
         * @param other Reservation whose sole scope ownership is transferred. @return This reservation.
         */
        AiDecisionTaskReservation &operator=(AiDecisionTaskReservation &&other) noexcept;
        AiDecisionTaskReservation(const AiDecisionTaskReservation &) = delete;
        AiDecisionTaskReservation &operator=(const AiDecisionTaskReservation &) = delete;
        /** @brief Cancels this scope before releasing its schema/agent lifetime metadata; safe on any thread. */
        ~AiDecisionTaskReservation();
        /** @brief Checks issuing-thread and agent/scope cancellation. @return False after move, retirement or off-owner use. */
        [[nodiscard]] bool CanAdmit() const noexcept;
        /** @brief Reads the captured agent/schema generation. @return Immutable owner-issued binding. */
        [[nodiscard]] const BlackboardInstanceBinding &Binding() const noexcept;
        /** @brief Reads the actual immutable schema publication. @return Metadata lease, empty after move. */
        [[nodiscard]] std::shared_ptr<const BlackboardSchema> Schema() const noexcept;
        /** @brief Reads the first exclusively reserved index. @return Index; no index is usable when Capacity is zero. */
        [[nodiscard]] std::uint32_t FirstSlot() const noexcept;
        /** @brief Reads the logical extent, not allocated mailbox/storage bytes. @return Reserved index count. */
        [[nodiscard]] std::uint32_t Capacity() const noexcept;
        /** @brief Borrows revocation without transferring reservation ownership. @return Agent-parented scope token. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;

    private:
        friend class AiSceneRuntime;
        /** @brief Captures a fully admitted reservation before its allocator high-water is published. */
        AiDecisionTaskReservation(BlackboardInstanceBinding binding, std::shared_ptr<const BlackboardSchema> schema, std::uint32_t first,
                                  std::uint32_t count, CancellationToken parent);
        BlackboardInstanceBinding binding_;
        std::shared_ptr<const BlackboardSchema> schema_;
        std::uint32_t first_{};
        std::uint32_t count_{};
        CancellationSource cancellation_;
        std::thread::id owner_;
    };
}  // namespace Horo::AI
