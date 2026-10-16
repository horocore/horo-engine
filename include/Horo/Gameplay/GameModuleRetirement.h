#pragma once
/** @file GameModuleRetirement.h @brief Explicit owner-lane retirement for application-composed gameplay candidates. */
#include "Horo/Gameplay/GameModuleHost.h"

#include <thread>

namespace Horo::Gameplay {
    /** @brief Bounded application lifetime authority; it outlives services and every outstanding reservation.
     * Close returns busy while native callbacks remain. Destroying an undrained queue violates the explicit lifetime precondition.
     */
    class GameModuleRetirementQueue final {
    public:
        /** @brief Preallocated admission guaranteeing that failure rollback cannot drop a busy native owner. */
        class Reservation final {
        public:
            /** @brief Transfers an unused admission. @param other Owner-lane reservation to consume. */
            Reservation(Reservation &&other) noexcept;
            Reservation &operator=(Reservation &&) = delete;
            Reservation(const Reservation &) = delete;
            /** @brief Releases unused admission. @pre Destroy or move only on the queue owner lane. */
            ~Reservation();
            /** @brief Transfers the candidate into its reserved slot.
             * @param owner Actual candidate, successful or failed.
             * @pre Called once on the queue owner lane; the native owner retains its services and guarded execution context.
             */
            void Retain(std::unique_ptr<LoadedGameModule> owner) noexcept;

        private:
            friend class GameModuleRetirementQueue;
            /** @brief Binds one preallocated slot. @param queue Outliving application owner. @param slot Reserved stable index. */
            Reservation(GameModuleRetirementQueue &queue, std::size_t slot) noexcept;
            GameModuleRetirementQueue *queue_{};
            std::size_t slot_{};
        };

        /** @brief Allocates bounded retirement storage before any gameplay callbacks. @param capacity Maximum concurrent retiring owners.
         */
        explicit GameModuleRetirementQueue(std::size_t capacity = 16);
        ~GameModuleRetirementQueue();
        /** @brief Reserves failure-path ownership before factories. @return Admission or a typed capacity/owner-lane error. */
        [[nodiscard]] Result<Reservation> Reserve();
        /** @brief Retries owner-lane teardown and releases resources only after native destruction. @return Reserved admission count, or
         * the original typed retirement cause; a failing owner stays retained. */
        [[nodiscard]] Result<std::size_t> Drain();
        /** @brief Drains without abandoning callbacks. @return Success only when no owner or reservation remains. */
        [[nodiscard]] Result<void> Close();

    private:
        struct Entry {
            bool reserved{};
            std::unique_ptr<LoadedGameModule> owner;
        };

        std::vector<Entry> entries_;
        std::thread::id thread_{std::this_thread::get_id()};
        bool draining_{};
    };
}  // namespace Horo::Gameplay
