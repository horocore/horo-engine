#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"
#include "PhysicsStateRuntimeInventory.h"

#include <atomic>
#include <exception>
#include <limits>

namespace Horo::Physics::Detail {
    /** @brief Storage lifetime and reader/publication authority are deliberately
     * distinct, both entirely hidden from consumers. */
    struct PhysicsStateSnapshotSlot final {
        static constexpr std::uint64_t Writing = std::numeric_limits<std::uint64_t>::max();
        std::shared_ptr<const PhysicsStateResourceRecord> resources; /**< Declared first, destroyed after snapshot values/code use.
                                                                      */
        std::unique_ptr<PhysicsStateSnapshot> snapshot;
        std::atomic<std::uint64_t> readers{};

        /** @brief Only zero real readers may be sealed; shared/weak storage ownership
         * is not reuse evidence. */
        [[nodiscard]] bool TryWrite() noexcept {
            std::uint64_t expected{};
            return readers.compare_exchange_strong(expected, Writing, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        /** @brief Failed capture leaves a private slot reusable without publishing
         * partial values. */
        void AbortWrite() noexcept {
            std::uint64_t expected = Writing;
            if (!readers.compare_exchange_strong(expected, 0, std::memory_order_release, std::memory_order_relaxed))
                std::terminate();
        }

        /** @brief Establishes one already-counted lease after every immutable value
         * and byte has been written. */
        void PublishWrite() noexcept {
            std::uint64_t expected = Writing;
            if (!readers.compare_exchange_strong(expected, 1, std::memory_order_release, std::memory_order_relaxed))
                std::terminate();
        }
    };
}  // namespace Horo::Physics::Detail
