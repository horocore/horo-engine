#pragma once

#include "CanonicalPhysicsStateBackend.h"

#include <thread>

namespace Horo::Physics::Detail {
    /** @brief Actual retained resource owner; runtime inventory is the last
     * reference and destroys it only on its owner lane. */
    struct PhysicsStateResourceRecord final {
        explicit PhysicsStateResourceRecord(std::thread::id owner) noexcept : ownerThread(owner) {}

        PhysicsStateResourceRecord(const PhysicsStateResourceRecord &) = delete;
        PhysicsStateResourceRecord &operator=(const PhysicsStateResourceRecord &) = delete;
        PhysicsStateResourceRecord(PhysicsStateResourceRecord &&) = delete;
        PhysicsStateResourceRecord &operator=(PhysicsStateResourceRecord &&) = delete;

        ~PhysicsStateResourceRecord();
        const std::thread::id ownerThread;
        PhysicsStateBuildBinding build;
        std::shared_ptr<const void> content;
        std::shared_ptr<const CanonicalPhysicsStateResources> native;
        std::size_t reservedPayloadBytes{};
    };

    /** @brief One same-runtime cold-reserved inventory; foreign value releases
     * never mutate its records, cursor or native globals. */
    class PhysicsStateRuntimeInventory final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<PhysicsStateRuntimeInventory>> Prepare(const PhysicsStateRuntimeBudget &budget,
                                                                                           std::size_t additionalFixedPayload = 0);
        [[nodiscard]] Result<void> Register(const std::shared_ptr<PhysicsStateResourceRecord> &record);
        /** @brief At most min(maximumRecords,initial record count) observations; busy
         * prefixes cannot starve later records. */
        [[nodiscard]] std::uint32_t Collect(std::uint32_t maximumRecords) noexcept;

        [[nodiscard]] std::uint32_t Count() const noexcept {
            return static_cast<std::uint32_t>(records_.size());
        }

        [[nodiscard]] std::size_t ReservedPayloadBytes() const noexcept {
            return reserved_;
        }

        [[nodiscard]] const PhysicsStateRuntimeBudget &Budget() const noexcept {
            return budget_;
        }

    private:
        PhysicsStateRuntimeBudget budget_;
        std::vector<std::shared_ptr<PhysicsStateResourceRecord>> records_;
        std::size_t reserved_{};
        std::size_t cursor_{};
    };
}  // namespace Horo::Physics::Detail
