#pragma once

namespace Horo::Physics::Detail {
    /** @brief Read-only native image-verification inputs; never installed/exported
     * as public SDK or capability evidence. */
    struct PhysicsStateImageAddresses final {
        const void *adapter{};
        const void *solver{};
    };

    /** @brief Actual canonical adapter implementation and JPH::RegisterTypes
     * addresses; native-OFF returns null inputs. */
    [[nodiscard]] PhysicsStateImageAddresses ReadPhysicsStateImageAddresses() noexcept;
}  // namespace Horo::Physics::Detail
