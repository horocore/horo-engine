#pragma once

#include <cstddef>

namespace Horo::Physics::Detail {
    /** @brief Conservative cumulative requested native bytes, not RSS, heap
     * overhead or a cold construction peak guarantee. */
    struct CanonicalPhysicsStateColdCost final {
        std::size_t requestedBytes{}, requests{};
        bool overflow{};
    };

    /** @brief Observes the EXISTING allocator on this joined cold owner only; never
     * replaces it or imposes an in-Jolt quota failure. */
    class CanonicalPhysicsStateColdCostScope final {
    public:
        explicit CanonicalPhysicsStateColdCostScope(CanonicalPhysicsStateColdCost &cost) noexcept;
        ~CanonicalPhysicsStateColdCostScope();
        CanonicalPhysicsStateColdCostScope(const CanonicalPhysicsStateColdCostScope &) = delete;
        CanonicalPhysicsStateColdCostScope &operator=(const CanonicalPhysicsStateColdCostScope &) = delete;

        [[nodiscard]] bool Admitted() const noexcept {
            return admitted_;
        }

    private:
        bool admitted_{};
    };

    /** @brief Adds actual delegated allocation payload; frees deliberately do not
     * reduce conservative retained reservation. */
    void RecordCanonicalPhysicsStateColdAllocation(std::size_t bytes) noexcept;
    /** @brief Prevents recorder/cold construction scope nesting; no second process
     * allocator authority is introduced. */
    [[nodiscard]] bool CanonicalPhysicsStateColdCostActive() noexcept;
}  // namespace Horo::Physics::Detail
